//===- SingletonVectorize.cpp - SAVIO Singleton Vectorize rewrite ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/SingletonVectorize.h"
#include "PartitionerUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
using namespace llvm::savio;

/// The llvm.savio.sv.* intrinsic of the in-scope operation \p I (see
/// isScalarCandidate in Partitioner.cpp).
static Intrinsic::ID svIntrinsicFor(const Instruction &I) {
  if (auto *C = dyn_cast<CallInst>(&I)) {
    switch (C->getIntrinsicID()) {
    case Intrinsic::ctpop:
      return Intrinsic::savio_sv_ctpop;
    case Intrinsic::ctlz:
      return Intrinsic::savio_sv_ctlz;
    case Intrinsic::smin:
      return Intrinsic::smin;
    case Intrinsic::smax:
      return Intrinsic::smax;
    case Intrinsic::umin:
      return Intrinsic::umin;
    case Intrinsic::umax:
      return Intrinsic::umax;
    case Intrinsic::fshl:
      return Intrinsic::fshl;
    case Intrinsic::fshr:
      return Intrinsic::fshr;
    default:
      llvm_unreachable("call is not in scope of Singleton Vectorize");
    }
  }
  switch (I.getOpcode()) {
  case Instruction::Add:
    return Intrinsic::savio_sv_add;
  case Instruction::Sub:
    return Intrinsic::savio_sv_sub;
  case Instruction::Mul:
    return Intrinsic::savio_sv_mul;
  case Instruction::And:
    return Intrinsic::savio_sv_and;
  case Instruction::Or:
    return Intrinsic::savio_sv_or;
  case Instruction::Xor:
    return Intrinsic::savio_sv_xor;
  case Instruction::Shl:
    return Intrinsic::savio_sv_shl;
  case Instruction::LShr:
    return Intrinsic::savio_sv_lshr;
  case Instruction::AShr:
    return Intrinsic::savio_sv_ashr;
  case Instruction::UDiv:
    return Intrinsic::savio_sv_udiv;
  case Instruction::URem:
    return Intrinsic::savio_sv_urem;
  default:
    llvm_unreachable("operation is not in scope of Singleton Vectorize");
  }
}

namespace {

class Rewriter {
  const Group &G;
  Loop *L;
  BasicBlock *BB;
  IRBuilder<> B;
  Type *EltTy;
  FixedVectorType *VecTy;
  Value *Lane0;

  /// Vector form of group operations, joined phis and boundary inputs.
  DenseMap<Value *, Value *> VecOf;

  Value *insertLane0(Value *Scalar) {
    return B.CreateInsertElement(PoisonValue::get(VecTy), Scalar, Lane0);
  }

  /// The vector form of the operand \p V of \p Op, which is not in the group.
  Value *enter(Instruction *Op, Value *V) {
    if (isa<Constant>(V)) {
      B.SetInsertPoint(Op);
      return insertLane0(V);
    }
    auto It = VecOf.find(V);
    if (It != VecOf.end())
      return It->second;

    InputKind K = classifyInput(L, G, V);
    BasicBlock *Preheader = L ? L->getLoopPreheader() : nullptr;
    Value *Vec;
    if (K == InputKind::Free && Preheader && L->isLoopInvariant(V)) {
      // Invariant: the transfer is done once, in the preheader.
      B.SetInsertPoint(Preheader->getTerminator());
      Vec = insertLane0(V);
    } else if (K == InputKind::LaneToLane0) {
      // The lane moves to lane 0 with a shuffle; classifyInput guarantees a
      // constant in-range lane of a fixed vector.
      auto *EE = cast<ExtractElementInst>(V);
      B.SetInsertPoint(Op);
      uint64_t Idx = cast<ConstantInt>(EE->getIndexOperand())->getZExtValue();
      SmallVector<int, 4> Mask(VecTy->getNumElements(), PoisonMaskElem);
      Mask[0] = int(Idx);
      Vec = B.CreateShuffleVector(EE->getVectorOperand(),
                                  PoisonValue::get(EE->getVectorOperand()
                                                       ->getType()),
                                  Mask);
    } else {
      // A load or GPR value: insert just before its first use.
      B.SetInsertPoint(Op);
      Vec = insertLane0(V);
    }
    VecOf[V] = Vec;
    return Vec;
  }

public:
  Rewriter(const Group &G, Loop *L)
      : G(G), L(L), BB(G.Ops.front()->getParent()), B(BB->getContext()),
        EltTy(G.Ops.front()->getType()),
        VecTy(FixedVectorType::get(EltTy, EltTy->isIntegerTy(64) ? 2 : 4)),
        Lane0(ConstantInt::get(EltTy, 0)) {}

  void run() {
    Module *M = BB->getModule();

    // Carried phis keep their value in vector form: the value entering from
    // outside the loop is inserted in lane 0 at the end of that predecessor.
    SmallVector<PHINode *, 2> VecPhis;
    for (PHINode *Phi : G.CarriedPhis) {
      PHINode *VP = PHINode::Create(VecTy, Phi->getNumIncomingValues(),
                                    Phi->getName() + ".sv", Phi->getIterator());
      VP->setDebugLoc(Phi->getDebugLoc());
      DenseMap<BasicBlock *, Value *> FromPred;
      for (unsigned I = 0, E = Phi->getNumIncomingValues(); I != E; ++I) {
        BasicBlock *Pred = Phi->getIncomingBlock(I);
        if (Pred == BB)
          continue; // The latch value is added once its operation exists.
        Value *&Entry = FromPred[Pred];
        if (!Entry) {
          B.SetInsertPoint(Pred->getTerminator());
          Entry = insertLane0(Phi->getIncomingValue(I));
        }
        VP->addIncoming(Entry, Pred);
      }
      VecOf[Phi] = VP;
      VecPhis.push_back(VP);
    }

    for (Instruction *Op : G.Ops) {
      Value *Args[3];
      // llvm.ctpop / llvm.ctlz have one data operand (the flag of llvm.ctlz
      // is an immediate that the one-operand llvm.savio.sv.ctlz does not
      // take); llvm.fshl / llvm.fshr have 3 operands;
      // every other operation (including smin/smax/umin/umax) is binary.
      unsigned NumArgs = !bitCountName(*Op).empty()
                             ? 1
                             : (isFunnelShift(*Op) ? 3 : 2);
      for (unsigned I = 0; I != NumArgs; ++I) {
        Value *V = Op->getOperand(I);
        // Arguments and constants are never in the group; they may be cached
        // in VecOf as inputs, which enter() reuses.
        auto *VI = dyn_cast<Instruction>(V);
        if (auto It = VecOf.find(V);
            It != VecOf.end() && VI && isInGroup(G, VI)) {
          Args[I] = It->second;
        } else if ((I == 1 && Op->isShift() && isa<ConstantInt>(V)) ||
                   (I == 2 && isFunnelShift(*Op) && isa<ConstantInt>(V))) {
          // Constant shift count: a splat the backend matches to an immediate.
          Args[I] = ConstantVector::getSplat(VecTy->getElementCount(),
                                             cast<ConstantInt>(V));
        } else {
          Args[I] = enter(Op, V);
        }
      }
      B.SetInsertPoint(Op);
      B.SetCurrentDebugLocation(Op->getDebugLoc());
      Function *Decl =
          Intrinsic::getOrInsertDeclaration(M, svIntrinsicFor(*Op), {VecTy});
      VecOf[Op] = B.CreateCall(Decl, ArrayRef(Args, NumArgs),
                               Op->getName() + ".sv");
    }

    // One incoming per edge from the latch: a switch may reach the header
    // through several cases.
    for (unsigned I = 0, E = G.CarriedPhis.size(); I != E; ++I) {
      PHINode *Phi = G.CarriedPhis[I];
      for (unsigned J = 0, N = Phi->getNumIncomingValues(); J != N; ++J)
        if (Phi->getIncomingBlock(J) == BB)
          VecPhis[I]->addIncoming(VecOf[Phi->getIncomingValue(J)], BB);
    }

    for (Instruction *Op : G.LiveOuts)
      leave(Op);

    // Erase the scalar group. What still uses it is itself erased.
    for (PHINode *Phi : G.CarriedPhis)
      Phi->replaceAllUsesWith(PoisonValue::get(Phi->getType()));
    for (Instruction *Op : G.Ops)
      Op->replaceAllUsesWith(PoisonValue::get(Op->getType()));
    SmallSetVector<ExtractElementInst *, 2> Extracts;
    for (Instruction *Op : G.Ops)
      for (Value *V : Op->operands())
        if (auto *EE = dyn_cast<ExtractElementInst>(V))
          Extracts.insert(EE);
    for (Instruction *Op : G.Ops)
      Op->eraseFromParent();
    for (PHINode *Phi : G.CarriedPhis)
      Phi->eraseFromParent();
    for (ExtractElementInst *EE : Extracts)
      if (EE->use_empty())
        EE->eraseFromParent();
  }

private:
  /// Gives the users of the live-out \p Op outside the group its scalar value.
  void leave(Instruction *Op) {
    Value *Vec = VecOf[Op];
    SmallVector<Use *, 4> Outside;
    for (Use &U : Op->uses())
      if (!isInGroup(G, U.getUser()))
        Outside.push_back(&U);

    if (!liveOutNeedsGpr(G, *Op)) {
      // Every user is a store: each stores lane 0 straight from the vector.
      for (Use *U : Outside) {
        auto *S = cast<StoreInst>(U->getUser());
        B.SetInsertPoint(S);
        B.SetCurrentDebugLocation(S->getDebugLoc());
        S->setOperand(0, B.CreateExtractElement(Vec, Lane0));
      }
      return;
    }

    // Users after the loop that are single-entry phis of another block take
    // the value in that block, outside the loop.
    auto IsExitPhi = [&](Use *U) {
      auto *P = dyn_cast<PHINode>(U->getUser());
      return P && !L->contains(P->getParent()) && P->getNumIncomingValues() == 1;
    };
    if (L && L->isInnermost() && L->getNumBlocks() == 1 &&
        all_of(Outside, IsExitPhi)) {
      DenseMap<BasicBlock *, Value *> PerBlock;
      for (Use *U : Outside) {
        auto *P = cast<PHINode>(U->getUser());
        Value *&E = PerBlock[P->getParent()];
        if (!E) {
          B.SetInsertPoint(P->getParent()->getFirstInsertionPt());
          B.SetCurrentDebugLocation(P->getDebugLoc());
          E = B.CreateExtractElement(Vec, Lane0);
        }
        P->replaceAllUsesWith(E);
        P->eraseFromParent();
      }
      return;
    }

    // One extract right after the operation serves every outside user.
    B.SetInsertPoint(cast<Instruction>(Vec)->getNextNode());
    B.SetCurrentDebugLocation(Op->getDebugLoc());
    Value *E = B.CreateExtractElement(Vec, Lane0);
    for (Use *U : Outside)
      U->set(E);
  }
};

} // namespace

/// The llvm.savio.fpdiv<Lanes>.<op> intrinsic, \p Lanes in {2, 4, 8, 16}.
static Intrinsic::ID fpDivIntrinsicFor(unsigned Lanes, bool Rem) {
  switch (Lanes) {
  case 2:
    return Rem ? Intrinsic::savio_fpdiv2_urem : Intrinsic::savio_fpdiv2_udiv;
  case 4:
    return Rem ? Intrinsic::savio_fpdiv4_urem : Intrinsic::savio_fpdiv4_udiv;
  case 8:
    return Rem ? Intrinsic::savio_fpdiv8_urem : Intrinsic::savio_fpdiv8_udiv;
  case 16:
    return Rem ? Intrinsic::savio_fpdiv16_urem : Intrinsic::savio_fpdiv16_udiv;
  }
  llvm_unreachable("no fpdiv intrinsic for this lane count");
}

void savio::rewriteVectorDivide(Instruction *I, unsigned GroupMask,
                                unsigned GroupLanes) {
  assert((I->getOpcode() == Instruction::UDiv ||
          I->getOpcode() == Instruction::URem) &&
         GroupMask && "not a vector divide to rewrite");
  // Only a prefix of the groups is ever chosen (the cost model cannot price a
  // later group without the first ones).
  if (GroupMask & (GroupMask + 1))
    report_fatal_error("savio: lane groups of a vector divide must be moved "
                       "from lane 0 on");
  bool Rem = I->getOpcode() == Instruction::URem;
  unsigned NumLanes = cast<FixedVectorType>(I->getType())->getNumElements();
  unsigned Lanes = llvm::popcount(GroupMask) * GroupLanes;
  Module *M = I->getModule();
  IRBuilder<> B(I);
  B.SetCurrentDebugLocation(I->getDebugLoc());
  Value *A = I->getOperand(0), *D = I->getOperand(1);

  // The first Lanes lanes divide the vector as it is (the other lanes of the
  // result are poison).
  Function *Decl =
      Intrinsic::getOrInsertDeclaration(M, fpDivIntrinsicFor(Lanes, Rem),
                                        {I->getType()});
  Value *Res = B.CreateCall(Decl, {A, D}, I->getName() + ".fp");
  // The lanes not sent through fdiv stay on the integer divider.
  for (unsigned Lane = Lanes; Lane != NumLanes; ++Lane) {
    Value *Idx = B.getInt32(Lane);
    Value *X = B.CreateExtractElement(A, Idx);
    Value *Y = B.CreateExtractElement(D, Idx);
    Value *S = Rem ? B.CreateURem(X, Y) : B.CreateUDiv(X, Y);
    Res = B.CreateInsertElement(Res, S, Idx);
  }
  I->replaceAllUsesWith(Res);
  I->eraseFromParent();
}

void savio::rewriteSingletonVectorize(const Group &G, Loop *L) {
  assert(!G.Ops.empty() && "empty group");
  Rewriter(G, L).run();
}
