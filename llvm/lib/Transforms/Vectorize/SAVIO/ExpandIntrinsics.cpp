//===- ExpandIntrinsics.cpp - savio-expand: lower llvm.savio.* to IR ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Expansions (every poison lane is a free choice, so the lowering may define it):
//
//  sv.<op> (not divide): the vector operation on all lanes. Lane 0 is exact;
//    the other lanes are poison in the intrinsic, so computing them is a
//    refinement. (Shifts: a lane whose count is out of range is poison in IR
//    too, which only touches that lane.)
//
//  sv.ctpop / sv.ctlz (one operand): llvm.ctpop / llvm.ctlz(x, false) of the
//    whole vector. Lane 0 is exactly the scalar result (ctlz of 0 is the
//    element width); the other lanes are poison in the intrinsic.
//
//  sv.udiv / sv.urem: lane 0 only, through double precision:
//    extract, uitofp i32 -> double, fdiv, fptoui, [urem: a - q*b], insert into
//    poison. A zero divisor gives inf/NaN, whose fptoui is poison in IR; that
//    is exactly the "lane 0 poison, never UB" the intrinsic specifies. (An
//    integer divide of the other lanes must not be formed: they may be zero.)
//
//  fpdiv<L>.udiv / urem: lanes 0..L-1 through double precision:
//    shuffle the first L lanes, uitofp <L x i32> -> <L x double> (exact),
//    fdiv, fptoui back, [urem: a - q*b on L lanes], widen with poison.
//
// Exactness of the floating-point divide for u32: a and b are below 2^32 and
// converted to double exactly. If b does not divide a, a/b is at least 1/b away
// from the next integer, while the rounding error of the division is at most
// (a/b) * 2^-53 < 2^32/b * 2^-53 = 2^-21/b, far smaller, so the rounded
// quotient never reaches an integer from below and fptoui truncation returns
// floor(a/b). If b divides a the quotient is an integer below 2^53, exactly
// representable. (The AArch64 sequence ucvtf -> fdiv -> fcvtzu relies on the
// same argument.)
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/ExpandIntrinsics.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;

#define DEBUG_TYPE "savio-expand"

namespace {

/// True for the one-operand sv forms (sv.ctpop, sv.ctlz, sv.cttz).
bool isUnaryCount(Intrinsic::ID ID) {
  return ID == Intrinsic::savio_sv_ctpop || ID == Intrinsic::savio_sv_ctlz ||
         ID == Intrinsic::savio_sv_cttz;
}

/// Binary opcode of an sv.<op> / fpdiv.<op> intrinsic; false if \p ID is not
/// one. \p Lanes is 0 for sv.*, L for fpdiv<L>.*. The one-operand count forms
/// are not binary: see isUnaryCount.
bool classify(Intrinsic::ID ID, Instruction::BinaryOps &Opc, unsigned &Lanes,
              bool &IsDiv) {
  Lanes = 0;
  IsDiv = false;
  switch (ID) {
#define SV(NAME, OPC)                                                          \
  case Intrinsic::savio_sv_##NAME:                                             \
    Opc = Instruction::OPC;                                                    \
    return true;
    SV(add, Add)
    SV(sub, Sub)
    SV(mul, Mul)
    SV(and, And)
    SV(or, Or)
    SV(xor, Xor)
    SV(shl, Shl)
    SV(lshr, LShr)
    SV(ashr, AShr)
#undef SV
  case Intrinsic::savio_sv_udiv:
    Opc = Instruction::UDiv;
    IsDiv = true;
    return true;
  case Intrinsic::savio_sv_urem:
    Opc = Instruction::URem;
    IsDiv = true;
    return true;
#define FPDIV(L)                                                               \
  case Intrinsic::savio_fpdiv##L##_udiv:                                       \
    Opc = Instruction::UDiv;                                                   \
    Lanes = L;                                                                 \
    IsDiv = true;                                                              \
    return true;                                                               \
  case Intrinsic::savio_fpdiv##L##_urem:                                       \
    Opc = Instruction::URem;                                                   \
    Lanes = L;                                                                 \
    IsDiv = true;                                                              \
    return true;
    FPDIV(2)
    FPDIV(4)
    FPDIV(8)
    FPDIV(16)
#undef FPDIV
  default:
    return false;
  }
}

Value *expandOne(IntrinsicInst *II, Instruction::BinaryOps Opc, unsigned Lanes,
                 bool IsDiv) {
  IRBuilder<> B(II);
  if (isUnaryCount(II->getIntrinsicID())) {
    Value *A = II->getArgOperand(0);
    if (II->getIntrinsicID() == Intrinsic::savio_sv_ctpop)
      return B.CreateUnaryIntrinsic(Intrinsic::ctpop, A);
    if (II->getIntrinsicID() == Intrinsic::savio_sv_ctlz)
      return B.CreateIntrinsic(Intrinsic::ctlz, {A->getType()},
                               {A, B.getFalse()});
    // sv.cttz: cttz(x, false) — is_zero_poison=false, matches ctlz behavior.
    return B.CreateIntrinsic(Intrinsic::cttz, {A->getType()},
                             {A, B.getFalse()});
  }
  Value *A = II->getArgOperand(0), *D = II->getArgOperand(1);
  auto *VecTy = cast<FixedVectorType>(II->getType());
  if (!IsDiv)
    return B.CreateBinOp(Opc, A, D);

  bool Rem = Opc == Instruction::URem;
  Type *I32 = B.getInt32Ty(), *F64 = B.getDoubleTy();
  if (Lanes == 0) {
    // sv.udiv / sv.urem: lane 0 as a scalar.
    Value *A0 = B.CreateExtractElement(A, uint64_t(0));
    Value *D0 = B.CreateExtractElement(D, uint64_t(0));
    Value *Q = B.CreateFPToUI(
        B.CreateFDiv(B.CreateUIToFP(A0, F64), B.CreateUIToFP(D0, F64)), I32);
    Value *R = Rem ? B.CreateSub(A0, B.CreateMul(Q, D0)) : Q;
    return B.CreateInsertElement(PoisonValue::get(VecTy), R, uint64_t(0));
  }

  // fpdiv<L>: lanes 0..L-1, at the width of the vector when L is all of it.
  unsigned N = VecTy->getNumElements();
  SmallVector<int, 16> Low(Lanes);
  for (unsigned I = 0; I != Lanes; ++I)
    Low[I] = I;
  auto Narrow = [&](Value *V) {
    return Lanes == N ? V : B.CreateShuffleVector(V, Low);
  };
  Value *AL = Narrow(A), *DL = Narrow(D);
  auto *DblTy = FixedVectorType::get(F64, Lanes);
  auto *IntTy = FixedVectorType::get(I32, Lanes);
  Value *Q = B.CreateFPToUI(
      B.CreateFDiv(B.CreateUIToFP(AL, DblTy), B.CreateUIToFP(DL, DblTy)),
      IntTy);
  Value *R = Rem ? B.CreateSub(AL, B.CreateMul(Q, DL)) : Q;
  if (Lanes == N)
    return R;
  SmallVector<int, 16> Widen(N, PoisonMaskElem);
  for (unsigned I = 0; I != Lanes; ++I)
    Widen[I] = I;
  return B.CreateShuffleVector(R, Widen);
}

/// True if \p M declares any llvm.savio.* function the pass could expand. A
/// module without one (stock llc input) skips the per-instruction scan.
bool moduleDeclaresSAVIO(const Module &M) {
  Instruction::BinaryOps Opc;
  unsigned Lanes;
  bool IsDiv;
  for (const Function &Fn : M)
    if (Fn.isIntrinsic() &&
        (isUnaryCount(Fn.getIntrinsicID()) ||
         classify(Fn.getIntrinsicID(), Opc, Lanes, IsDiv)))
      return true;
  return false;
}

bool expandFunction(Function &F, const SAVIOKeepFn &Keep) {
  SmallVector<std::pair<IntrinsicInst *, Value *>, 8> Work;
  for (Instruction &I : instructions(F)) {
    auto *II = dyn_cast<IntrinsicInst>(&I);
    if (!II)
      continue;
    Instruction::BinaryOps Opc = Instruction::Add;
    unsigned Lanes = 0;
    bool IsDiv = false;
    if (!isUnaryCount(II->getIntrinsicID()) &&
        !classify(II->getIntrinsicID(), Opc, Lanes, IsDiv))
      continue;
    if (Keep && Keep(II->getIntrinsicID()))
      continue;
    Work.push_back({II, expandOne(II, Opc, Lanes, IsDiv)});
  }
  for (auto &[II, New] : Work) {
    II->replaceAllUsesWith(New);
    II->eraseFromParent();
  }
  return !Work.empty();
}

class SAVIOExpandIntrinsicsLegacyPass : public FunctionPass {
  SAVIOKeepFn Keep;
  bool HasSAVIO = true;

public:
  static char ID;
  SAVIOExpandIntrinsicsLegacyPass(SAVIOKeepFn Keep = nullptr)
      : FunctionPass(ID), Keep(std::move(Keep)) {
    initializeSAVIOExpandIntrinsicsLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }
  bool doInitialization(Module &M) override {
    HasSAVIO = moduleDeclaresSAVIO(M);
    return false;
  }
  bool runOnFunction(Function &F) override {
    return HasSAVIO && expandFunction(F, Keep);
  }
  StringRef getPassName() const override { return "SAVIO expand intrinsics"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};
char SAVIOExpandIntrinsicsLegacyPass::ID = 0;

} // end anonymous namespace

INITIALIZE_PASS(SAVIOExpandIntrinsicsLegacyPass, DEBUG_TYPE,
                "SAVIO expand intrinsics", false, false)

PreservedAnalyses SAVIOExpandIntrinsicsPass::run(Function &F,
                                                 FunctionAnalysisManager &) {
  // The module scan is cached per module: the pass runs once per function and
  // llvm.savio.* declarations are never added once codegen has started.
  if (CachedModule != F.getParent()) {
    CachedModule = F.getParent();
    HasSAVIO = moduleDeclaresSAVIO(*CachedModule);
  }
  if (!HasSAVIO || !expandFunction(F, Keep))
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

FunctionPass *llvm::createSAVIOExpandIntrinsicsPass(SAVIOKeepFn Keep) {
  return new SAVIOExpandIntrinsicsLegacyPass(std::move(Keep));
}
