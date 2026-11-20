//===- OperationCosts.cpp - SAVIO operation-level costs -------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

// Operation-level costs are kept separate from assignment graph construction
// so alternative cost models can replace this module without touching search.
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "CostModelUtils.h"
#include "llvm/Transforms/Vectorize/SAVIO/Availability.h"
#include "PartitionerUtils.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/Transforms/Vectorize/SAVIO/RegionCost.h"
#include "llvm/Transforms/Vectorize/SAVIO/SearchCore.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <cfloat>
#include <optional>
#include <tuple>
#include <utility>

using namespace llvm;
using namespace llvm::savio;

namespace llvm {
namespace savio {

#define DEBUG_TYPE "savio"

unsigned laneCount(const Group &G) {
  return cast<FixedVectorType>(G.Ops[0]->getType())->getNumElements();
}

/// "v4i32" / "v8i32" / "v2i64" ... (vector form) or "i32" / "i64" (scalar lane
/// form).
std::string laneTypeName(const Group &G, bool Vector) {
  auto *VT = cast<FixedVectorType>(G.Ops[0]->getType());
  return typeKey(Vector ? static_cast<Type *>(VT) : VT->getElementType());
}

/// The shift of \p U's operands that the backend folds into \p U (shifted-
/// register form, or USRA/SSRA for the vector add of a right shift): a shift
/// of the group by a constant amount whose only user is \p U. The shifted
/// operand of `sub` must be the second. Null if none. \p Vector selects the
/// vector form, in which only `add` of a right shift fuses.
const Instruction *fusibleShift(const Instruction &U, const Group &G,
                                       bool Vector) {
  switch (U.getOpcode()) {
  case Instruction::Add:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    break;
  case Instruction::Sub:
    if (Vector)
      return nullptr;
    break;
  default:
    return nullptr;
  }
  for (unsigned K : {1u, 0u}) {
    if (K == 0 && U.getOpcode() == Instruction::Sub)
      break;
    auto *S = dyn_cast<Instruction>(U.getOperand(K));
    if (!S || !isShift(S->getOpcode()) || !S->hasOneUse() ||
        !is_contained(G.Ops, S) || !isImmAmount(S->getOperand(1)))
      continue;
    if (Vector && !(U.getOpcode() == Instruction::Add &&
                    S->getOpcode() != Instruction::Shl))
      continue;
    return S;
  }
  return nullptr;
}

/// Cost of the operation \p I of the ToScalar group \p G in its vector form
/// (\p Vector, as LLVM left it) or as one scalar operation of a lane (not yet
/// multiplied by the lane count). A shift by a constant uses the ".imm" key. A
/// shift folded into its user is charged once, with the fused key, on the user
/// (the shift then costs nothing); if the port-info has no fused key the two
/// operations are charged separately. Null if a needed key is missing.
std::optional<OpCost> scalarizeOpCost(const PortDescription &PD,
                                              const Instruction &I,
                                              const Group &G, bool Vector) {
  std::string Ty = laneTypeName(G, Vector);
  auto Fused = [&](const Instruction &U, const Instruction &S) {
    return (Twine(opName(U)) + "." + opName(S) + ".imm " + Ty).str();
  };
  // A shift folded into its (only) user.
  if (isShift(I.getOpcode()) && I.hasOneUse())
    if (auto *U = dyn_cast<Instruction>(*I.user_begin()))
      if (is_contained(G.Ops, U) && fusibleShift(*U, G, Vector) == &I &&
          PD.irOpCost(Fused(*U, I)))
        return OpCost{{}, 0, 0};
  if (const Instruction *S = fusibleShift(I, G, Vector))
    if (std::optional<OpCost> C = PD.irOpCost(Fused(I, *S)))
      return C;
  bool Imm = isShift(I.getOpcode()) && isImmAmount(I.getOperand(1));
  return PD.irOpCost(
      (Twine(opName(I)) + (Imm ? ".imm " : " ") + Ty).str());
}

/// Cost of \p Lanes copies of \p C issued side by side: resource use and
/// micro-ops add up, latency does not (the lanes are independent).
OpCost lanes(OpCost C, unsigned Lanes) {
  for (ResourceUse &U : C.Uses)
    U.Cycles *= Lanes;
  C.Uops *= Lanes;
  return C;
}

/// True if the operand \p V of the Scalarized group \p G reaches its lanes
/// without a lane extract: a constant (splat or not: immediates), a vector load
/// the slice owns (split into scalar loads), or, in a loop with a preheader, a
/// loop-invariant value (its lanes are extracted once in the preheader).
bool scalarizeInputIsFree(const Loop *L, const Group &G,
                                 const Value *V) {
  if (isa<Constant>(V))
    return true;
  if (auto *LD = dyn_cast<LoadInst>(V))
    if (is_contained(G.OwnedLoads, LD))
      return true;
  return L && L->getLoopPreheader() && L->isLoopInvariant(V);
}


} // namespace savio
} // namespace llvm
