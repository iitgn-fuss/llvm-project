//===- Availability.cpp - SAVIO per-transformation key requirements -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Availability.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"

using namespace llvm;
using namespace llvm::savio;

StringRef savio::transformName(Transform X) {
  switch (X) {
  case Transform::SingletonVectorize:
    return "singleton-vectorize";
  case Transform::FpDivideLanes:
    return "fpdiv-lanes";
  case Transform::Scalarize:
    return "scalarize";
  }
  llvm_unreachable("unknown transformation");
}

SmallVector<std::string, 8> savio::requiredKeys(Transform X, StringRef Op,
                                                Type *Ty, unsigned L) {
  SmallVector<std::string, 8> Keys;
  switch (X) {
  case Transform::SingletonVectorize:
    Keys.push_back(("sv." + Op + " v4i32").str());
    Keys.push_back("gpr_to_vec");
    Keys.push_back("vec_to_gpr");
    break;
  case Transform::FpDivideLanes: {
    std::string T = typeKey(Ty);
    Keys.push_back((Op + " " + T).str());
    Keys.push_back(("fpdiv" + Twine(L) + "." + Op + " " + T).str());
    break;
  }
  case Transform::Scalarize: {
    std::string V = typeKey(Ty);
    std::string S = typeKey(cast<VectorType>(Ty)->getElementType());
    Keys.push_back((Op + " " + V).str());
    Keys.push_back((Op + " " + S).str());
    for (StringRef Mem : {"load", "store"}) {
      Keys.push_back((Mem + " " + V).str());
      Keys.push_back((Mem + " " + S).str());
    }
    Keys.push_back("lane_extract");
    Keys.push_back("lane_insert");
    break;
  }
  }
  return Keys;
}

std::optional<std::string> savio::firstMissing(const PortDescription &PD,
                                               ArrayRef<std::string> Keys) {
  static const std::pair<StringRef, TransferKind> Transfers[] = {
      {"gpr_to_vec", TransferKind::GprToVec},
      {"vec_to_gpr", TransferKind::VecToGpr},
      {"lane_extract", TransferKind::LaneExtract},
      {"lane_insert", TransferKind::LaneInsert}};
  for (const std::string &K : Keys) {
    const auto *T = find_if(Transfers, [&](auto &P) { return P.first == K; });
    bool Have = T != std::end(Transfers) ? PD.transferCost(T->second).has_value()
                                         : PD.irOpCost(K).has_value();
    if (!Have)
      return K;
  }
  return std::nullopt;
}
