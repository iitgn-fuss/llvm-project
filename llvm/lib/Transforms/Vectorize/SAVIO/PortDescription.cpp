//===- PortDescription.cpp - SAVIO execution-port description -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/IOSandbox.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::savio;

namespace {

Error makeError(const Twine &File, const Twine &Msg) {
  return createStringError(inconvertibleErrorCode(), File + ": " + Msg);
}

/// Reads and parses one JSON file that must hold a top-level object.
Expected<json::Object> readObject(StringRef Dir, StringRef Name) {
  SmallString<128> Path(Dir);
  sys::path::append(Path, Name);
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf)
    return makeError(Name, "cannot read " + Path + ": " +
                               Buf.getError().message());
  Expected<json::Value> V = json::parse((*Buf)->getBuffer());
  if (!V)
    return makeError(Name, "invalid JSON: " + toString(V.takeError()));
  json::Object *O = V->getAsObject();
  if (!O)
    return makeError(Name, "top level must be an object");
  return std::move(*O);
}

Error requireProvenance(StringRef File, const json::Object &O,
                        const Twine &Where) {
  for (StringRef K : {"source", "evidence"}) {
    std::optional<StringRef> S = O.getString(K);
    if (!S || S->empty())
      return makeError(File, Where + " is missing key '" + K + "'");
  }
  return Error::success();
}

/// Sums the costs of Parts into one cost, merging uses of the same class.
OpCost sumCosts(ArrayRef<const OpCost *> Parts) {
  OpCost Sum{{}, 0, 0};
  for (const OpCost *P : Parts) {
    for (const ResourceUse &U : P->Uses) {
      auto *It = find_if(Sum.Uses, [&](const ResourceUse &S) {
        return S.Class == U.Class;
      });
      if (It == Sum.Uses.end())
        Sum.Uses.push_back(U);
      else
        It->Cycles += U.Cycles;
    }
    Sum.Latency += P->Latency;
    Sum.Uops += P->Uops;
  }
  return Sum;
}

/// Resolves a JSON array of opcode names against Ops.
Expected<OpCost> costOfOpcodes(StringRef File, const Twine &Where,
                               const json::Value *V,
                               const StringMap<OpCost> &Ops) {
  const json::Array *A = V ? V->getAsArray() : nullptr;
  if (!A)
    return makeError(File, Where + " must be an array of opcode names");
  SmallVector<const OpCost *, 4> Parts;
  for (const json::Value &E : *A) {
    std::optional<StringRef> Name = E.getAsString();
    if (!Name)
      return makeError(File, Where + " holds a non-string opcode");
    auto It = Ops.find(*Name);
    if (It == Ops.end())
      return makeError(File, Where + " names unknown opcode '" + *Name +
                                 "' (not in ops.json)");
    Parts.push_back(&It->second);
  }
  return sumCosts(Parts);
}

} // namespace

std::string llvm::savio::archDirFor(const Triple &T) {
  return T.isAArch64() ? "AArch64" : "";
}

Expected<std::unique_ptr<PortDescription>>
PortDescription::load(StringRef Root, StringRef Arch, StringRef CPU) {
  // Reading the port-info files from inside the optimizer is intended (clang
  // enables the IO sandbox around the pipeline).
  auto BypassSandbox = sys::sandbox::scopedDisable();
  SmallString<128> Dir(Root);
  sys::path::append(Dir, Arch, CPU);
  if (!sys::fs::is_directory(Dir))
    return errorCodeToError(errc::no_such_file_or_directory);

  std::unique_ptr<PortDescription> PD(new PortDescription());

  // resources.json
  {
    Expected<json::Object> R = readObject(Dir, "resources.json");
    if (!R)
      return R.takeError();
    constexpr StringLiteral F = "resources.json";
    if (Error E = requireProvenance(F, *R, "top level"))
      return std::move(E);
    std::optional<int64_t> W = R->getInteger("dispatch_width");
    if (!W || *W <= 0)
      return makeError(F, "'dispatch_width' must be a positive integer");
    PD->DispatchWidth = *W;
    if (const json::Value *WV = R->get("window_uops")) {
      const json::Object *WO = WV->getAsObject();
      if (!WO)
        return makeError(F, "'window_uops' must be an object");
      if (Error E = requireProvenance(F, *WO, "'window_uops'"))
        return std::move(E);
      std::optional<int64_t> V = WO->getInteger("value");
      if (!V || *V <= 0)
        return makeError(F, "'window_uops' needs a positive integer 'value'");
      PD->WindowUops = *V;
    }
    const json::Object *Classes = R->getObject("classes");
    if (!Classes || Classes->empty())
      return makeError(F, "'classes' must be a non-empty object");
    // The object is unordered; sort the names so class ids are deterministic.
    SmallVector<std::pair<std::string, unsigned>, 8> Sorted;
    for (const auto &KV : *Classes) {
      const json::Object *C = KV.second.getAsObject();
      std::optional<int64_t> U = C ? C->getInteger("units") : std::nullopt;
      if (!U || *U <= 0)
        return makeError(F, "class '" + Twine(StringRef(KV.first)) +
                                "' needs a positive integer 'units'");
      Sorted.emplace_back(std::string(StringRef(KV.first)), *U);
    }
    llvm::sort(Sorted);
    for (auto &[Name, Units] : Sorted) {
      PD->ClassNames.push_back(Name);
      PD->ClassUnits.push_back(Units);
    }
  }

  auto ClassId = [&](StringRef Name) -> std::optional<unsigned> {
    auto *It = find(PD->ClassNames, Name);
    if (It == PD->ClassNames.end())
      return std::nullopt;
    return It - PD->ClassNames.begin();
  };

  // ops.json
  StringMap<OpCost> Ops;
  {
    Expected<json::Object> O = readObject(Dir, "ops.json");
    if (!O)
      return O.takeError();
    constexpr StringLiteral F = "ops.json";
    const json::Array *List = O->getArray("ops");
    if (!List)
      return makeError(F, "'ops' must be an array");
    for (const json::Value &E : *List) {
      const json::Object *Op = E.getAsObject();
      std::optional<StringRef> Name =
          Op ? Op->getString("opcode") : std::nullopt;
      if (!Name)
        return makeError(F, "entry without string key 'opcode'");
      std::string Where = ("opcode '" + *Name + "'").str();
      if (Error Err = requireProvenance(F, *Op, Where))
        return std::move(Err);
      std::optional<int64_t> Lat = Op->getInteger("latency");
      std::optional<int64_t> Uops = Op->getInteger("uops");
      if (!Lat || *Lat < 0 || !Uops || *Uops < 0)
        return makeError(F, Where + " needs non-negative integer 'latency' "
                                    "and 'uops'");
      const json::Array *Uses = Op->getArray("uses");
      if (!Uses)
        return makeError(F, Where + " is missing array key 'uses'");
      OpCost C{{}, unsigned(*Lat), unsigned(*Uops)};
      for (const json::Value &UV : *Uses) {
        const json::Object *U = UV.getAsObject();
        std::optional<StringRef> Cls =
            U ? U->getString("class") : std::nullopt;
        std::optional<double> Cyc = U ? U->getNumber("cycles") : std::nullopt;
        if (!Cls || !Cyc || *Cyc < 0)
          return makeError(F, Where + " has a 'uses' entry without 'class' "
                                      "and non-negative 'cycles'");
        std::optional<unsigned> Id = ClassId(*Cls);
        if (!Id)
          return makeError(F, Where + " uses unknown class '" + *Cls +
                                  "' (not in resources.json)");
        C.Uses.push_back({*Id, *Cyc});
      }
      if (!Ops.try_emplace(*Name, std::move(C)).second)
        return makeError(F, "duplicate opcode '" + *Name + "'");
    }
  }

  // ops.json "sequences" (optional): whole-sequence measurements that
  // override the per-opcode sum for a probe key. Keys are validated against
  // probes.json below.
  StringMap<OpCost> Sequences;
  StringMap<ConcurrencyTable> Tables;
  {
    Expected<json::Object> O = readObject(Dir, "ops.json");
    if (!O)
      return O.takeError();
    constexpr StringLiteral F = "ops.json";
    if (const json::Value *SV = O->get("sequences")) {
      const json::Array *Seqs = SV->getAsArray();
      if (!Seqs)
        return makeError(F, "'sequences' must be an array");
      for (const json::Value &E : *Seqs) {
        const json::Object *S = E.getAsObject();
        std::optional<StringRef> Key =
            S ? S->getString("key") : std::nullopt;
        if (!Key)
          return makeError(F, "sequence entry without string key 'key'");
        std::string Where = ("sequence '" + *Key + "'").str();
        if (Error Err = requireProvenance(F, *S, Where))
          return std::move(Err);
        std::optional<int64_t> Lat = S->getInteger("latency");
        std::optional<int64_t> Uops = S->getInteger("uops");
        if (!Lat || *Lat < 0 || !Uops || *Uops < 0)
          return makeError(F, Where + " needs non-negative integer "
                                      "'latency' and 'uops'");
        const json::Array *Uses = S->getArray("uses");
        if (!Uses)
          return makeError(F, Where + " is missing array key 'uses'");
        OpCost C{{}, unsigned(*Lat), unsigned(*Uops)};
        for (const json::Value &UV : *Uses) {
          const json::Object *U = UV.getAsObject();
          std::optional<StringRef> Cls =
              U ? U->getString("class") : std::nullopt;
          std::optional<double> Cyc =
              U ? U->getNumber("cycles") : std::nullopt;
          if (!Cls || !Cyc || *Cyc < 0)
            return makeError(F, Where + " has a 'uses' entry without "
                                        "'class' and non-negative 'cycles'");
          std::optional<unsigned> Id = ClassId(*Cls);
          if (!Id)
            return makeError(F, Where + " uses unknown class '" + *Cls +
                                    "' (not in resources.json)");
          C.Uses.push_back({*Id, *Cyc});
        }
        if (const json::Value *TV = S->get("throughput_by_concurrency")) {
          const json::Array *TA = TV->getAsArray();
          ConcurrencyTable T;
          bool Ok = TA && !TA->empty();
          for (const json::Value &PV : TA ? *TA : json::Array()) {
            const json::Array *P = PV.getAsArray();
            std::optional<int64_t> M =
                P && P->size() == 2 ? (*P)[0].getAsInteger() : std::nullopt;
            std::optional<double> Cyc =
                P && P->size() == 2 ? (*P)[1].getAsNumber() : std::nullopt;
            if (!M || *M <= 0 || !Cyc || *Cyc <= 0 ||
                (!T.Points.empty() && T.Points.back().first >= *M)) {
              Ok = false;
              break;
            }
            T.Points.emplace_back(unsigned(*M), *Cyc);
          }
          if (!Ok)
            return makeError(F, Where + " 'throughput_by_concurrency' must be "
                                        "a non-empty array of [m, cycles], m "
                                        "strictly ascending and positive, "
                                        "cycles positive");
          Tables.try_emplace(*Key, std::move(T));
        }
        if (!Sequences.try_emplace(*Key, std::move(C)).second)
          return makeError(F, "duplicate " + Where);
      }
    }
  }

  // transfers.json
  {
    Expected<json::Object> T = readObject(Dir, "transfers.json");
    if (!T)
      return T.takeError();
    static constexpr std::pair<StringLiteral, TransferKind> Kinds[] = {
        {"gpr_to_vec", TransferKind::GprToVec},
        {"vec_to_gpr", TransferKind::VecToGpr},
        {"load_to_vec", TransferKind::LoadToVec},
        {"store_from_vec", TransferKind::StoreFromVec},
        {"lane_to_lane0", TransferKind::LaneToLane0}};
    for (const auto &[Key, Kind] : Kinds) {
      Expected<OpCost> C =
          costOfOpcodes("transfers.json", "key '" + Key + "'", T->get(Key), Ops);
      if (!C)
        return C.takeError();
      PD->Transfers[unsigned(Kind)] = std::move(*C);
    }
  }

  // probes.json
  {
    Expected<json::Object> P = readObject(Dir, "probes.json");
    if (!P)
      return P.takeError();
    const json::Object *Probes = P->getObject("probes");
    if (!Probes)
      return makeError("probes.json", "'probes' must be an object");
    for (const auto &KV : *Probes) {
      StringRef Key = KV.first;
      Expected<OpCost> C = costOfOpcodes("probes.json", "probe '" + Key + "'",
                                         &KV.second, Ops);
      if (!C)
        return C.takeError();
      PD->Probes.try_emplace(Key, std::move(*C));
    }
  }

  // A sequence entry replaces the opcode sum of its probe key.
  for (auto &KV : Sequences) {
    auto It = PD->Probes.find(KV.first());
    if (It == PD->Probes.end())
      return makeError("ops.json", "sequence '" + KV.first() +
                                       "' names an unknown probe key "
                                       "(not in probes.json)");
    It->second = std::move(KV.second);
    auto TIt = Tables.find(KV.first());
    if (TIt != Tables.end())
      PD->Concurrency.try_emplace(KV.first(), std::move(TIt->second));
  }

  return std::move(PD);
}

std::optional<OpCost> PortDescription::irOpCost(StringRef ProbeKey) const {
  auto It = Probes.find(ProbeKey);
  if (It == Probes.end())
    return std::nullopt;
  return It->second;
}

std::optional<OpCost> PortDescription::transferCost(TransferKind K) const {
  return Transfers[unsigned(K)];
}

const ConcurrencyTable *
PortDescription::concurrency(StringRef ProbeKey) const {
  auto It = Concurrency.find(ProbeKey);
  return It == Concurrency.end() ? nullptr : &It->second;
}

double ConcurrencyTable::at(double M) const {
  assert(!Points.empty() && "empty concurrency table");
  if (M <= Points.front().first)
    return Points.front().second;
  if (M >= Points.back().first)
    return Points.back().second;
  for (unsigned I = 1; I != Points.size(); ++I) {
    if (M > Points[I].first)
      continue;
    auto [M0, C0] = Points[I - 1];
    auto [M1, C1] = Points[I];
    return C0 + (C1 - C0) * (M - M0) / double(M1 - M0);
  }
  return Points.back().second;
}
