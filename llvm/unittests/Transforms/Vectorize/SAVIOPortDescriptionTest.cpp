//===- SAVIOPortDescriptionTest.cpp - SAVIO port description tests -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::savio;

namespace {

std::string inputsRoot() {
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  return std::string(Root);
}

std::optional<unsigned> classId(const PortDescription &PD, StringRef Name) {
  for (unsigned C = 0; C != PD.numClasses(); ++C)
    if (PD.className(C) == Name)
      return C;
  return std::nullopt;
}

/// Copies the test2s1v fixture to <tmp>/AArch64/test2s1v and returns <tmp>.
std::string copyFixture() {
  SmallString<128> Tmp;
  EXPECT_FALSE(sys::fs::createUniqueDirectory("savio-pd", Tmp));
  SmallString<128> Dst(Tmp);
  sys::path::append(Dst, "AArch64", "test2s1v");
  EXPECT_FALSE(sys::fs::create_directories(Dst));
  for (StringRef F :
       {"resources.json", "ops.json", "transfers.json", "probes.json"}) {
    SmallString<128> Src(inputsRoot());
    sys::path::append(Src, "AArch64", "test2s1v", F);
    SmallString<128> To(Dst);
    sys::path::append(To, F);
    EXPECT_FALSE(sys::fs::copy_file(Src, To));
  }
  return std::string(Tmp);
}

void rewrite(StringRef Root, StringRef File, StringRef From, StringRef To) {
  SmallString<128> P(Root);
  sys::path::append(P, "AArch64", "test2s1v", File);
  auto Buf = MemoryBuffer::getFile(P);
  ASSERT_TRUE(!!Buf);
  std::string S = (*Buf)->getBuffer().str();
  size_t Pos = S.find(From);
  ASSERT_NE(Pos, std::string::npos);
  S.replace(Pos, From.size(), To);
  std::error_code EC;
  raw_fd_ostream OS(P, EC);
  ASSERT_FALSE(EC);
  OS << S;
}

std::string loadError(StringRef Root) {
  auto PD = PortDescription::load(Root, "AArch64", "test2s1v");
  EXPECT_FALSE(!!PD);
  return PD ? "" : toString(PD.takeError());
}

TEST(SAVIOPortDescription, LoadsFixture) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  auto Scalar = classId(**PD, "scalar"), Vector = classId(**PD, "vector");
  ASSERT_TRUE(Scalar && Vector);
  EXPECT_EQ((*PD)->units(*Scalar), 2u);
  EXPECT_EQ((*PD)->units(*Vector), 1u);
  EXPECT_EQ((*PD)->dispatchWidth(), 4u);
  EXPECT_EQ((*PD)->numClasses(), 4u);
}

TEST(SAVIOPortDescription, ProbeSumsOpcodes) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  unsigned Div = *classId(**PD, "int_div"), Scalar = *classId(**PD, "scalar");
  std::optional<OpCost> C = (*PD)->irOpCost("urem i32");
  ASSERT_TRUE(C.has_value());
  ASSERT_EQ(C->Uses.size(), 2u);
  for (const ResourceUse &U : C->Uses) {
    if (U.Class == Div)
      EXPECT_EQ(U.Cycles, 4.0);
    else {
      EXPECT_EQ(U.Class, Scalar);
      EXPECT_EQ(U.Cycles, 1.0);
    }
  }
  EXPECT_EQ(C->Latency, 10u);

  std::optional<OpCost> V = (*PD)->irOpCost("sv.udiv v4i32");
  ASSERT_TRUE(V.has_value());
  EXPECT_EQ(V->Latency, 14u);
  EXPECT_TRUE((*PD)->irOpCost("sv.shl.imm v4i32").has_value());
  EXPECT_FALSE((*PD)->irOpCost("add i64").has_value());
}

TEST(SAVIOPortDescription, Transfers) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  std::optional<OpCost> G = (*PD)->transferCost(TransferKind::GprToVec);
  ASSERT_TRUE(G.has_value());
  EXPECT_EQ(G->Latency, 1u);
  ASSERT_EQ(G->Uses.size(), 1u);
  EXPECT_EQ(G->Uses[0].Class, *classId(**PD, "vector"));
  std::optional<OpCost> L = (*PD)->transferCost(TransferKind::LoadToVec);
  ASSERT_TRUE(L.has_value());
  EXPECT_TRUE(L->Uses.empty());
  EXPECT_EQ(L->Latency, 0u);
}

TEST(SAVIOPortDescription, LaneTransfersOptional) {
  // A Part-2 format directory has no lane_extract / lane_insert: it loads, and
  // Scalarize boundaries simply cannot be costed.
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  EXPECT_FALSE((*PD)->transferCost(TransferKind::LaneExtract).has_value());
  EXPECT_FALSE((*PD)->transferCost(TransferKind::LaneInsert).has_value());
  EXPECT_TRUE((*PD)->transferCost(TransferKind::GprToVec).has_value());
}

TEST(SAVIOPortDescription, LaneTransfersLoaded) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test3sc");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  unsigned Vector = *classId(**PD, "vector");
  std::optional<OpCost> E = (*PD)->transferCost(TransferKind::LaneExtract);
  ASSERT_TRUE(E.has_value());
  EXPECT_EQ(E->Latency, 3u);
  ASSERT_EQ(E->Uses.size(), 1u);
  EXPECT_EQ(E->Uses[0].Class, Vector);
  std::optional<OpCost> I = (*PD)->transferCost(TransferKind::LaneInsert);
  ASSERT_TRUE(I.has_value());
  EXPECT_EQ(I->Latency, 2u);
  // The five Part-2 kinds are unchanged.
  EXPECT_TRUE((*PD)->transferCost(TransferKind::LaneToLane0).has_value());
}

TEST(SAVIOPortDescription, LaneTransferUnknownOpcodeIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "transfers.json", "\"lane_to_lane0\"",
          "\"lane_extract\": [\"NOPE\"],\n  \"lane_to_lane0\"");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("lane_extract"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("NOPE"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, MissingCPUIsNotFound) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "nope");
  ASSERT_FALSE(!!PD);
  std::error_code EC = errorToErrorCode(PD.takeError());
  EXPECT_EQ(EC, std::make_error_code(std::errc::no_such_file_or_directory));
}

TEST(SAVIOPortDescription, UnknownClassIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"class\": \"scalar\"", "\"class\": \"bogus\"");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("ops.json"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("bogus"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, MissingSourceIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"source\"", "\"sauce\"");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("ops.json"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("source"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, UnknownProbeOpcodeIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "probes.json", "\"T_ADD\"", "\"T_NOPE\"");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("probes.json"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("T_NOPE"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

const char *SeqJson =
    "\"sequences\": [{\"key\": \"sv.urem v4i32\", \"latency\": 20, "
    "\"uops\": 7, \"uses\": [{\"class\": \"vector\", \"cycles\": 12}], "
    "\"source\": \"measured\", \"evidence\": \"unit test\"}],\n  \"ops\": [";

TEST(SAVIOPortDescription, SequenceOverridesSum) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"ops\": [", SeqJson);
  auto PD = PortDescription::load(Root, "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  std::optional<OpCost> C = (*PD)->irOpCost("sv.urem v4i32");
  ASSERT_TRUE(C.has_value());
  EXPECT_EQ(C->Latency, 20u);
  EXPECT_EQ(C->Uops, 7u);
  ASSERT_EQ(C->Uses.size(), 1u);
  EXPECT_EQ(C->Uses[0].Class, *classId(**PD, "vector"));
  EXPECT_EQ(C->Uses[0].Cycles, 12.0);
  // Other keys still sum their opcodes.
  EXPECT_EQ((*PD)->irOpCost("urem i32")->Latency, 10u);
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, SequenceAbsentFallsBack) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  std::optional<OpCost> C = (*PD)->irOpCost("sv.urem v4i32");
  ASSERT_TRUE(C.has_value());
  // Part 1 format: the sum over the probe's opcodes (T_VUDIV_FP + T_VUDIV_FIX).
  EXPECT_EQ(C->Latency, 14u);
  EXPECT_EQ(C->Uops, 2u);
  ASSERT_EQ(C->Uses.size(), 2u);
  for (const ResourceUse &U : C->Uses) {
    if (U.Class == *classId(**PD, "fp_div"))
      EXPECT_EQ(U.Cycles, 4.0);
    else {
      EXPECT_EQ(U.Class, *classId(**PD, "vector"));
      EXPECT_EQ(U.Cycles, 3.0);
    }
  }
}

TEST(SAVIOPortDescription, SequenceUnknownKeyIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"ops\": [",
          "\"sequences\": [{\"key\": \"nope v4i32\", \"latency\": 1, "
          "\"uops\": 1, \"uses\": [], \"source\": \"measured\", "
          "\"evidence\": \"x\"}],\n  \"ops\": [");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("nope v4i32"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, SequenceMissingEvidenceIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"ops\": [", SeqJson);
  rewrite(Root, "ops.json", "\"evidence\": \"unit test\"", "\"x\": 1");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("evidence"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, ConcurrencyTableInterpolatesAndClamps) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1vc");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  EXPECT_EQ((*PD)->windowUops(), 12u);
  const ConcurrencyTable *T = (*PD)->concurrency("sv.udiv v4i32");
  ASSERT_NE(T, nullptr);
  EXPECT_DOUBLE_EQ(T->at(1), 8.0);
  EXPECT_DOUBLE_EQ(T->at(2), 6.0);
  EXPECT_DOUBLE_EQ(T->at(3), 5.0);    // midway between (2,6) and (4,4)
  EXPECT_DOUBLE_EQ(T->at(1.5), 7.0);
  EXPECT_DOUBLE_EQ(T->at(4), 4.0);
  EXPECT_DOUBLE_EQ(T->at(0.25), 8.0); // clamped below
  EXPECT_DOUBLE_EQ(T->at(100), 4.0);   // clamped above
  // The entry's own cost is still its `uses` (the m = 1 value here).
  EXPECT_EQ((*PD)->irOpCost("sv.udiv v4i32")->Uops, 4u);
  EXPECT_EQ((*PD)->concurrency("sv.add v4i32"), nullptr);
}

TEST(SAVIOPortDescription, NoWindowOrTableInPart1Format) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  EXPECT_EQ((*PD)->windowUops(), 0u);
  EXPECT_EQ((*PD)->concurrency("sv.udiv v4i32"), nullptr);
}

TEST(SAVIOPortDescription, BadConcurrencyTableIsError) {
  for (const char *Bad : {"[[2, 6], [1, 8]]", "[[1, 8], [1, 7]]", "[[0, 8]]",
                          "[[1, -1]]", "[]", "[[1]]"}) {
    std::string Root = copyFixture();
    rewrite(Root, "ops.json", "\"ops\": [",
            (Twine("\"sequences\": [{\"key\": \"sv.urem v4i32\", \"latency\": 1, "
                   "\"uops\": 1, \"uses\": [], \"throughput_by_concurrency\": ") +
             Bad + ", \"source\": \"measured\", \"evidence\": \"x\"}],\n  "
             "\"ops\": [").str());
    std::string Msg = loadError(Root);
    EXPECT_NE(Msg.find("throughput_by_concurrency"), std::string::npos)
        << Bad << ": " << Msg;
    sys::fs::remove_directories(Root);
  }
}

TEST(SAVIOPortDescription, ConcurrencyTableNeedsExactlyOneUse) {
  for (const char *Uses :
       {"[]", "[{\"class\": \"vector\", \"cycles\": 1}, "
              "{\"class\": \"scalar\", \"cycles\": 1}]"}) {
    std::string Root = copyFixture();
    rewrite(Root, "ops.json", "\"ops\": [",
            (Twine("\"sequences\": [{\"key\": \"sv.urem v4i32\", \"latency\": 1, "
                   "\"uops\": 1, \"uses\": ") + Uses +
             ", \"throughput_by_concurrency\": [[1, 8]], \"source\": "
             "\"measured\", \"evidence\": \"x\"}],\n  \"ops\": [").str());
    std::string Msg = loadError(Root);
    EXPECT_NE(Msg.find("exactly one"), std::string::npos) << Uses << ": " << Msg;
    sys::fs::remove_directories(Root);
  }
}

TEST(SAVIOPortDescription, VectorWidthBits) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  EXPECT_EQ((*PD)->vectorWidthBits(), 128u); // Absent: 128.

  for (unsigned Bits : {128u, 256u, 512u}) {
    std::string Root = copyFixture();
    rewrite(Root, "resources.json", "\"dispatch_width\"",
            ("\"vector_width_bits\": " + Twine(Bits) + ", \"dispatch_width\"")
                .str());
    auto Loaded = PortDescription::load(Root, "AArch64", "test2s1v");
    ASSERT_TRUE(!!Loaded) << toString(Loaded.takeError());
    EXPECT_EQ((*Loaded)->vectorWidthBits(), Bits);
    sys::fs::remove_directories(Root);
  }
  for (const char *Bad : {"384", "64", "0", "\"512\"", "256.5"}) {
    std::string Root = copyFixture();
    rewrite(Root, "resources.json", "\"dispatch_width\"",
            (Twine("\"vector_width_bits\": ") + Bad + ", \"dispatch_width\"")
                .str());
    std::string Msg = loadError(Root);
    EXPECT_NE(Msg.find("vector_width_bits"), std::string::npos)
        << Bad << ": " << Msg;
    sys::fs::remove_directories(Root);
  }
}

TEST(SAVIOPortDescription, TypeKey) {
  LLVMContext Ctx;
  Type *I32 = Type::getInt32Ty(Ctx), *I64 = Type::getInt64Ty(Ctx);
  EXPECT_EQ(typeKey(I32), "i32");
  EXPECT_EQ(typeKey(I64), "i64");
  EXPECT_EQ(typeKey(FixedVectorType::get(I32, 4)), "v4i32");
  EXPECT_EQ(typeKey(FixedVectorType::get(I32, 8)), "v8i32");
  EXPECT_EQ(typeKey(FixedVectorType::get(I32, 16)), "v16i32");
  EXPECT_EQ(typeKey(FixedVectorType::get(I64, 2)), "v2i64");
  EXPECT_EQ(typeKey(FixedVectorType::get(I64, 4)), "v4i64");
  EXPECT_EQ(typeKey(Type::getInt16Ty(Ctx)), "");
  EXPECT_EQ(typeKey(Type::getFloatTy(Ctx)), "");
}

TEST(SAVIOPortDescription, ArchDir) {
  EXPECT_EQ(archDirFor(Triple("arm64-apple-macos")), "AArch64");
  EXPECT_EQ(archDirFor(Triple("aarch64-linux-gnu")), "AArch64");
  EXPECT_EQ(archDirFor(Triple("x86_64-linux-gnu")), "x86");
  EXPECT_EQ(archDirFor(Triple("i386-linux-gnu")), "");
  EXPECT_EQ(archDirFor(Triple("riscv64-unknown-linux-gnu")), "");
}

} // namespace
