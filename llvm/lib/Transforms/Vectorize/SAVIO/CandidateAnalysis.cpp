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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>

using namespace llvm;
using namespace llvm::savio;

#define DEBUG_TYPE "savio"

static cl::opt<bool> RelaxPoisonFlags(
    "savio-relax-poison-flags", cl::init(true),
    cl::desc("Allow singleton vectorization of instructions with poison-generating flags (nsw/nuw/exact)"));

namespace llvm {
namespace savio {

/// Returns true if instruction \p I carries undefined/poison-generating flags
/// such as `nuw` (no unsigned wrap), `nsw` (no signed wrap), or `exact`.
///
/// WHY: SAVIO's singleton vector intrinsics (e.g. `llvm.savio.sv.add`) do not
/// provide poison-flag attributes or operands. Silently dropping `nsw`, `nuw`,
/// or `exact` during vectorization would convert poisoned inputs into valid
/// wrapped results, violating refinement semantics. We conservatively disqualify
/// any instruction with poison flags.
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

/// Identifies scalar operations eligible for Singleton Vectorization:
/// 32-bit and 64-bit integer arithmetic (add, sub, mul), bitwise logic
/// (and, or, xor), bit shifts (shl, lshr, ashr), and bit-counting calls
/// (llvm.ctpop, llvm.ctlz). 32-bit integer divide/remainder (udiv, urem)
/// are supported; 64-bit divide/remainder are strictly excluded for exactness.
///
/// Disqualifies instructions that carry poison flags or non-constant ctlz
/// zero-poison flags.
bool isScalarCandidate(const Instruction &I) {
  Type *Ty = I.getType();
  bool IsI32 = Ty->isIntegerTy(32);
  bool IsI64 = Ty->isIntegerTy(64);
  if (!IsI32 && !IsI64)
    return false;
  if (!RelaxPoisonFlags && hasPoisonFlag(I))
    return false;
  // Calls: llvm.ctpop has 1 operand; llvm.ctlz has (val, is_zero_poison).
  // The lane-0 vector ctlz intrinsic always produces bitwidth for zero input,
  // which is exact regardless of the is_zero_poison flag value.
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
    return true;
  case Instruction::UDiv:
  case Instruction::URem:
    // UDiv and URem are strictly restricted to i32: 32-bit integer division
    // through double-precision float divide is exact, but 64-bit integer
    // division exceeds float64's 53-bit mantissa and would lose precision.
    return IsI32;
  default:
    return false;
  }
}

/// Identifies vector operations eligible for Scalarization:
/// Fixed-width integer vectors (128, 256, or 512 bits) of i32 or i64 elements
/// performing arithmetic, logical, or shift operations. Vector divide/remainder
/// instructions are excluded here because they are handled by the FP-divider
/// lane-group path instead.
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
