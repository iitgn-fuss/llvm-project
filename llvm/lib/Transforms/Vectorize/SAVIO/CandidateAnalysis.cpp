//===- CandidateAnalysis.cpp - SAVIO candidate eligibility ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Transforms/Vectorize/SAVIO/Availability.h"
#include "PartitionerUtils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>

using namespace llvm;
using namespace llvm::savio;

#define DEBUG_TYPE "savio"

namespace llvm {
namespace savio {
static bool hasPoisonFlag(const Instruction &I) {
  if (isa<OverflowingBinaryOperator>(&I))
    if (I.hasNoUnsignedWrap() || I.hasNoSignedWrap())
      return true;
  switch (I.getOpcode()) {
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::UDiv:
  case Instruction::SDiv:
    return I.isExact();
  default:
    return false;
  }
}
/// Singleton Vectorize candidates: i32 add sub mul and or xor shl lshr ashr
/// udiv urem, and calls to llvm.ctpop.i32 / llvm.ctlz.i32 (the latter's
/// lane-0 form defines the zero case as 32, which is exact for both values of
/// is_zero_poison; the flag is an immediate, so it is always a constant).
bool isScalarCandidate(const Instruction &I) {
  if (!I.getType()->isIntegerTy(32))
    return false;
  if (hasPoisonFlag(I))
    return false;
  if (StringRef BC = bitCountName(I); !BC.empty())
    return BC == "ctpop" || isa<ConstantInt>(I.getOperand(1));
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::UDiv:
  case Instruction::URem:
    return true;
  default:
    return false;
  }
}

/// Scalarize candidates: add sub mul and or xor shl lshr ashr on fixed vectors
/// of i32 or i64 that are 128, 256 or 512 bits wide (whether the port-info has
/// the keys is decided at candidacy, by isAvailable). Vector udiv/urem stay
/// with the lane-group path.
bool isVectorCandidate(const Instruction &I) {
  auto *VT = dyn_cast<FixedVectorType>(I.getType());
  if (!VT || !(VT->getElementType()->isIntegerTy(32) ||
               VT->getElementType()->isIntegerTy(64)))
    return false;
  uint64_t Bits = VT->getPrimitiveSizeInBits().getFixedValue();
  if (Bits != 128 && Bits != 256 && Bits != 512)
    return false;
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    return true;
  default:
    return false;
  }
}

bool isCandidate(const Instruction &I) {
  return isScalarCandidate(I) || isVectorCandidate(I);
}

/// Whether the transformation \p X of \p I has every port-info key it needs
/// (see requiredKeys); no description (\p PD null) means no check. Logs the
/// keys looked up, or the first one missing.
bool isAvailable(const PortDescription *PD, Transform X,
                 const Instruction &I, unsigned L) {
  if (!PD)
    return true;
  std::string Op = opName(I);
  if (isShift(I.getOpcode()) &&
      (X == Transform::SingletonVectorize ? isa<ConstantInt>(I.getOperand(1))
                                          : isImmAmount(I.getOperand(1))))
    Op += ".imm";
  SmallVector<std::string, 8> Keys = requiredKeys(X, Op, I.getType(), L);
  std::optional<std::string> Missing = firstMissing(
      *PD, Keys, X == Transform::Scalarize ? typeKey(I.getType()) : "");
  LLVM_DEBUG({
    dbgs() << "savio: " << transformName(X) << " of " << opName(I) << ' '
           << *I.getType();
    if (Missing) {
      dbgs() << " not available: missing '" << *Missing << "'\n";
    } else {
      dbgs() << " keys:";
      for (const std::string &K : Keys)
        dbgs() << " '" << K << "'";
      dbgs() << "\n";
    }
  });
  return !Missing;
}

} // namespace savio
} // namespace llvm
