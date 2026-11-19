//===- PartitionerUtils.h - Shared SAVIO partitioner helpers ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONERUTILS_H
#define LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONERUTILS_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Vectorize/SAVIO/RegionCost.h"
#include <optional>

namespace llvm {
namespace savio {

inline bool isShift(unsigned Opcode) {
  return Opcode == Instruction::Shl || Opcode == Instruction::LShr ||
         Opcode == Instruction::AShr;
}

inline StringRef bitCountName(const Instruction &I) {
  if (!I.getType()->isIntegerTy(32))
    return "";
  switch (I.getOpcode() == Instruction::Call
              ? cast<CallInst>(I).getIntrinsicID()
              : Intrinsic::not_intrinsic) {
  case Intrinsic::ctpop:
    return "ctpop";
  case Intrinsic::ctlz:
    return "ctlz";
  default:
    return "";
  }
}

inline std::string opName(const Instruction &I) {
  StringRef BitCount = bitCountName(I);
  return BitCount.empty() ? std::string(I.getOpcodeName()) : BitCount.str();
}

inline iterator_range<User::const_op_iterator>
dataOperands(const Instruction &I) {
  if (!bitCountName(I).empty())
    return make_range(I.op_begin(), I.op_begin() + 1);
  return I.operands();
}

inline bool isImmAmount(const Value *V) {
  auto *C = dyn_cast<Constant>(V);
  return C && (isa<ConstantInt>(C) || C->getSplatValue());
}

std::optional<RegionEstimate>
estimateAssignment(const Region &R, const RegionGraph &RG,
                   const PortDescription &PD, FallbackCostFn Fallback,
                   ArrayRef<bool> Promoted, unsigned Copies);

} // namespace savio
} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONERUTILS_H
