//===- PartitionerUtils.h - Shared SAVIO partitioner helpers ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private utility helpers shared across SAVIO partitioning, candidate
/// eligibility analysis, and assignment costing.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONERUTILS_H
#define LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONERUTILS_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Vectorize/SAVIO/Availability.h"
#include "llvm/Transforms/Vectorize/SAVIO/RegionCost.h"
#include <optional>

namespace llvm {
namespace savio {

/// Returns true if \p Opcode is an integer shift instruction (shl, lshr, ashr).
inline bool isShift(unsigned Opcode) {
  return Opcode == Instruction::Shl || Opcode == Instruction::LShr ||
         Opcode == Instruction::AShr;
}

/// Identifies supported intrinsic operations (`ctpop`, `ctlz`, `smin`, `smax`,
/// `umin`, `umax`), returning their canonical key name.
/// Returns empty StringRef for any other instruction.
inline StringRef supportedIntrinsicName(const Instruction &I) {
  Type *ScalarTy = I.getType()->getScalarType();
  if (!ScalarTy->isIntegerTy(32) && !ScalarTy->isIntegerTy(64))
    return "";
  switch (I.getOpcode() == Instruction::Call
              ? cast<CallInst>(I).getIntrinsicID()
              : Intrinsic::not_intrinsic) {
  case Intrinsic::ctpop:
    return "ctpop";
  case Intrinsic::ctlz:
    return "ctlz";
  case Intrinsic::smin:
    return "smin";
  case Intrinsic::smax:
    return "smax";
  case Intrinsic::umin:
    return "umin";
  case Intrinsic::umax:
    return "umax";
  case Intrinsic::fshl:
    return "fshl";
  case Intrinsic::fshr:
    return "fshr";
  default:
    return "";
  }
}

/// Identifies supported bit-counting intrinsics (`llvm.ctpop.i32`,
/// `llvm.ctpop.i64`, `llvm.ctlz.i32`, or `llvm.ctlz.i64`), returning their
/// canonical key name ("ctpop" / "ctlz").
/// Returns empty StringRef for any other instruction.
inline StringRef bitCountName(const Instruction &I) {
  StringRef Name = supportedIntrinsicName(I);
  return (Name == "ctpop" || Name == "ctlz") ? Name : "";
}

/// Returns true if intrinsic call \p I computes integer min/max.
inline bool isMinMax(const Instruction &I) {
  StringRef Name = supportedIntrinsicName(I);
  return Name == "smin" || Name == "smax" || Name == "umin" || Name == "umax";
}

/// Returns true if intrinsic call \p I computes funnel shift / rotate.
inline bool isFunnelShift(const Instruction &I) {
  StringRef Name = supportedIntrinsicName(I);
  return Name == "fshl" || Name == "fshr";
}

/// Returns the operation name for port-info matching: the intrinsic key name
/// ("ctpop", "ctlz", "smin", etc.) for intrinsics, or the IR opcode name for standard instructions.
inline std::string opName(const Instruction &I) {
  StringRef Intrin = supportedIntrinsicName(I);
  return Intrin.empty() ? std::string(I.getOpcodeName()) : Intrin.str();
}

/// Returns an iterator range over the meaningful data operands of instruction \p I.
/// For unary bit counts (ctpop/ctlz), only the primary data operand (operand 0) is returned,
/// ignoring any trailing flag operand.
/// For binary min/max calls and funnel shifts, operand 0 and operand 1 are both data operands.
inline iterator_range<User::const_op_iterator>
dataOperands(const Instruction &I) {
  if (isMinMax(I) || isFunnelShift(I))
    return make_range(I.op_begin(), I.op_begin() + 2);
  if (!bitCountName(I).empty())
    return make_range(I.op_begin(), I.op_begin() + 1);
  return I.operands();
}

/// Returns true if \p V represents a compile-time constant integer or splatted
/// vector constant suitable for immediate shift amounts.
inline bool isImmAmount(const Value *V) {
  auto *C = dyn_cast<Constant>(V);
  return C && (isa<ConstantInt>(C) || C->getSplatValue());
}

/// Returns true if scalar instruction \p I is an eligible candidate for
/// Singleton Vectorization (scalar -> vector lane 0).
bool isScalarCandidate(const Instruction &I);

/// Returns true if vector instruction \p I is an eligible candidate for
/// Scalarization (vector -> individual scalar operations).
bool isVectorCandidate(const Instruction &I);

/// Returns true if instruction \p I is either a scalar or vector candidate.
bool isCandidate(const Instruction &I);

/// Returns true if the target's port-info contains all required opcode and
/// transfer keys to model transformation \p X on instruction \p I.
bool isAvailable(const PortDescription *PD, Transform X,
                 const Instruction &I, unsigned L = 0);

/// Evaluates the cost of a proposed assignment of candidate groups across
/// virtual unroll copies, returning the computed execution bounds.
std::optional<RegionEstimate>
estimateAssignment(const Region &R, const RegionGraph &RG,
                   const PortDescription &PD, FallbackCostFn Fallback,
                   ArrayRef<bool> Promoted, unsigned Copies);

} // namespace savio
} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONERUTILS_H
