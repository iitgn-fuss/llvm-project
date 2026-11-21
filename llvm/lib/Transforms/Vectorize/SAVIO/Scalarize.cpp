//===- Scalarize.cpp - SAVIO Scalarize rewrite ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Scalarize.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Alignment.h"

using namespace llvm;
using namespace llvm::savio;

namespace {

class Rewriter {
  const Group &G;
  Loop *L;
  BasicBlock *BB;
  IRBuilder<> B;
  FixedVectorType *VecTy;
  Type *EltTy;
  unsigned N;
  uint64_t EltBytes;

  /// The scalar value of each lane of each group operation and owned load.
  DenseMap<Value *, SmallVector<Value *, 4>> Lanes;
  /// Lane extracts of other inputs, made once.
  DenseMap<std::pair<Value *, unsigned>, Value *> Extracted;

  /// Pointer to byte offset \p Off from \p Ptr, inserted before \p At.
  Value *lanePtr(Value *Ptr, uint64_t Off, Instruction *At) {
    if (Off == 0)
      return Ptr;
    B.SetInsertPoint(At);
    return B.CreateGEP(B.getInt8Ty(), Ptr, B.getInt64(Off), "lane.ptr");
  }

  static void copyAliasing(Instruction *From, Instruction *To) {
    static const unsigned Kinds[] = {LLVMContext::MD_tbaa,
                                     LLVMContext::MD_alias_scope,
                                     LLVMContext::MD_noalias,
                                     LLVMContext::MD_access_group};
    for (unsigned K : Kinds)
      if (MDNode *MD = From->getMetadata(K))
        To->setMetadata(K, MD);
  }

  /// Lane \p K of the operand \p V of \p Op, which is not an operation of the
  /// group (or is one, or an owned load).
  Value *lane(Value *V, unsigned K, Instruction *Op) {
    auto It = Lanes.find(V);
    if (It != Lanes.end())
      return It->second[K];
    if (auto *C = dyn_cast<Constant>(V)) {
      if (Constant *Elt = C->getAggregateElement(K))
        return Elt;
      B.SetInsertPoint(Op);
      return B.CreateExtractElement(C, B.getInt64(K));
    }
    Value *&E = Extracted[{V, K}];
    if (E)
      return E;
    // A loop-invariant vector is extracted once in the preheader; any other
    // one just before its first use (it dominates every later one).
    BasicBlock *Preheader = L ? L->getLoopPreheader() : nullptr;
    if (Preheader && L->isLoopInvariant(V))
      B.SetInsertPoint(Preheader->getTerminator());
    else
      B.SetInsertPoint(Op);
    return E = B.CreateExtractElement(V, B.getInt64(K));
  }

public:
  Rewriter(const Group &G, Loop *L)
      : G(G), L(L), BB(G.Ops.front()->getParent()), B(BB->getContext()),
        VecTy(cast<FixedVectorType>(G.Ops.front()->getType())),
        EltTy(VecTy->getElementType()), N(VecTy->getNumElements()),
        EltBytes(EltTy->getPrimitiveSizeInBits() / 8) {}

  void run() {
    // Owned loads: N scalar loads where the vector load was.
    for (LoadInst *LD : G.OwnedLoads) {
      auto &V = Lanes[LD];
      for (unsigned K = 0; K != N; ++K) {
        Value *P = lanePtr(LD->getPointerOperand(), K * EltBytes, LD);
        B.SetInsertPoint(LD);
        auto *S = B.CreateAlignedLoad(
            EltTy, P, commonAlignment(LD->getAlign(), K * EltBytes));
        copyAliasing(LD, cast<Instruction>(S));
        V.push_back(S);
      }
    }

    // The operations, in program order, one per lane at the vector one's
    // place.
    for (Instruction *I : G.Ops) {
      auto &V = Lanes[I];
      if (auto *SI = dyn_cast<SelectInst>(I)) {
        for (unsigned K = 0; K != N; ++K) {
          Value *Cond = lane(SI->getCondition(), K, I);
          Value *TrueVal = lane(SI->getTrueValue(), K, I);
          Value *FalseVal = lane(SI->getFalseValue(), K, I);
          auto *S = SelectInst::Create(Cond, TrueVal, FalseVal, "",
                                       I->getIterator());
          S->copyIRFlags(I);
          S->setDebugLoc(I->getDebugLoc());
          V.push_back(S);
        }
      } else if (auto *II = dyn_cast<IntrinsicInst>(I)) {
        Intrinsic::ID IID = II->getIntrinsicID();
        Function *Decl =
            Intrinsic::getOrInsertDeclaration(BB->getModule(), IID, {EltTy});
        for (unsigned K = 0; K != N; ++K) {
          SmallVector<Value *, 3> Args;
          for (unsigned ArgIdx = 0; ArgIdx != II->arg_size(); ++ArgIdx) {
            Value *Arg = II->getArgOperand(ArgIdx);
            if (isa<ConstantInt>(Arg) && Arg->getType()->isIntegerTy(1) &&
                (IID == Intrinsic::ctlz || IID == Intrinsic::cttz ||
                 IID == Intrinsic::abs)) {
              Args.push_back(Arg);
            } else if (isa<ConstantInt>(Arg) && !Arg->getType()->isVectorTy()) {
              Args.push_back(Arg);
            } else {
              Args.push_back(lane(Arg, K, I));
            }
          }
          auto *S = CallInst::Create(Decl, Args, "", I->getIterator());
          S->copyIRFlags(I);
          S->setDebugLoc(I->getDebugLoc());
          V.push_back(S);
        }
      } else if (auto *BO = dyn_cast<BinaryOperator>(I)) {
        for (unsigned K = 0; K != N; ++K) {
          Value *A = lane(I->getOperand(0), K, I);
          Value *Bv = lane(I->getOperand(1), K, I);
          auto *S = BinaryOperator::Create(BO->getOpcode(), A, Bv, "",
                                           I->getIterator());
          S->copyIRFlags(I);
          S->setDebugLoc(I->getDebugLoc());
          V.push_back(S);
        }
      } else {
        llvm_unreachable("unexpected instruction in scalarize group");
      }
    }

    // Owned stores: lane by lane where the vector store was.
    for (StoreInst *ST : G.OwnedStores) {
      auto &V = Lanes[cast<Instruction>(ST->getValueOperand())];
      for (unsigned K = 0; K != N; ++K) {
        Value *P = lanePtr(ST->getPointerOperand(), K * EltBytes, ST);
        B.SetInsertPoint(ST);
        auto *S = B.CreateAlignedStore(
            V[K], P, commonAlignment(ST->getAlign(), K * EltBytes));
        copyAliasing(ST, S);
      }
    }

    // Live-outs with a user that is not an owned store get the vector back.
    for (Instruction *I : G.LiveOuts) {
      if (!liveOutNeedsInsert(G, *I))
        continue;
      B.SetInsertPoint(I);
      Value *Vec = PoisonValue::get(VecTy);
      for (unsigned K = 0; K != N; ++K)
        Vec = B.CreateInsertElement(Vec, Lanes[I][K], B.getInt64(K));
      I->replaceUsesWithIf(Vec, [&](Use &U) {
        return !isInGroup(G, U.getUser());
      });
    }

    for (StoreInst *ST : G.OwnedStores)
      ST->eraseFromParent();
    for (Instruction *I : reverse(G.Ops))
      I->eraseFromParent();
    for (LoadInst *LD : G.OwnedLoads)
      LD->eraseFromParent();
  }
};

} // namespace

void savio::rewriteScalarize(const Group &G, Loop *L) {
  assert(G.Dir == Direction::ToScalar && "not a Scalarize group");
  Rewriter(G, L).run();
}
