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

static cl::opt<bool> AllowConstantDivisors(
    "savio-allow-constant-divisors", cl::init(false),
    cl::desc("Allow singleton vectorization of divisions with constant divisors (which are otherwise lowered to reciprocal multiply)"));

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
  // llvm.smin / llvm.smax / llvm.umin / llvm.umax have 2 data operands and no poison flags.
  // llvm.fshl / llvm.fshr have 3 operands: op0, op1, and constant shift amount.
  if (StringRef IN = supportedIntrinsicName(I); !IN.empty()) {
    if (IN == "ctpop" || IN == "smin" || IN == "smax" || IN == "umin" ||
        IN == "umax")
      return true;
    if (IN == "fshl" || IN == "fshr")
      return isa<ConstantInt>(I.getOperand(2));
    return isa<ConstantInt>(I.getOperand(1));
  }
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
  case Instruction::Select:
    return true;
  case Instruction::UDiv:
  case Instruction::URem:
    // Reject constant divisors unless explicitly allowed: scalar division or
    // remainder by a compile-time constant is lowered to reciprocal multiplication
    // (imul/mulx, 1-cycle throughput on Port 1) without using the hardware divider.
    // Vectorizing to floating-point division (vdivsd) would replace a 3-cycle
    // integer multiply with a 20+ cycle floating-point division and vector transfers.
    if (!AllowConstantDivisors && isa<Constant>(I.getOperand(1)))
      return false;
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
  case Instruction::Select:
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
