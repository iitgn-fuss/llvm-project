//===- SAVIOPartitionerTest.cpp - SAVIO operand classification tests ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::savio;

namespace {

class SAVIOClassifyInputTest : public testing::Test {
protected:
  LLVMContext Ctx;
  std::unique_ptr<Module> M;

  Function *parse(StringRef IR) {
    SMDiagnostic Err;
    M = parseAssemblyString(IR, Err, Ctx);
    if (!M) {
      std::string Msg;
      raw_string_ostream OS(Msg);
      Err.print("SAVIOClassifyInputTest", OS);
      ADD_FAILURE() << Msg;
      return nullptr;
    }
    return M->getFunction("f");
  }

  static Instruction *find(Function &F, StringRef Name) {
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (I.getName() == Name)
          return &I;
    return nullptr;
  }

  static Group groupOf(std::initializer_list<Instruction *> Ops) {
    Group G;
    for (Instruction *I : Ops)
      G.Ops.push_back(I);
    return G;
  }
};

TEST_F(SAVIOClassifyInputTest, Constant) {
  Function *F = parse(R"(
    define i32 @f(i32 %a) {
      %g = add i32 %a, 7
      ret i32 %g
    })");
  ASSERT_TRUE(F);
  Group G = groupOf({find(*F, "g")});
  Value *C = find(*F, "g")->getOperand(1);
  EXPECT_EQ(classifyInput(nullptr, G, C), InputKind::Free);
}

TEST_F(SAVIOClassifyInputTest, ArgumentOutsideLoop) {
  Function *F = parse(R"(
    define i32 @f(i32 %a, i32 %b) {
      %g = add i32 %a, %b
      ret i32 %g
    })");
  ASSERT_TRUE(F);
  Group G = groupOf({find(*F, "g")});
  EXPECT_EQ(classifyInput(nullptr, G, F->getArg(0)), InputKind::GprToVec);
}

static const char *LoopIR = R"(
  define void @f(i32 %a, <4 x i32> %vec, i32 %idx, ptr %p, ptr %q, i64 %n) {
  entry:
    %inv = add i32 %a, 1
    br label %loop
  loop:
    %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
    %ld = load i32, ptr %p
    %ld2 = load i32, ptr %q
    %e2 = extractelement <4 x i32> %vec, i32 2
    %evar = extractelement <4 x i32> %vec, i32 %idx
    %eoob = extractelement <4 x i32> %vec, i32 7
    %g = add i32 %ld, %a
    %g2 = add i32 %ld2, %ld2
    %g3 = add i32 %inv, %e2
    %g4 = add i32 %evar, %eoob
    %i.next = add i64 %i, 1
    %c = icmp ult i64 %i.next, %n
    br i1 %c, label %loop, label %exit
  exit:
    ret void
  })";

TEST_F(SAVIOClassifyInputTest, InLoopWithPreheader) {
  Function *F = parse(LoopIR);
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  Loop *L = LI.getLoopFor(find(*F, "g")->getParent());
  ASSERT_TRUE(L);
  ASSERT_TRUE(L->getLoopPreheader());
  Group G = groupOf({find(*F, "g")});

  // An argument is loop-invariant: the transfer is hoisted.
  EXPECT_EQ(classifyInput(L, G, F->getArg(0)), InputKind::Free);
  // An invariant instruction defined before the loop.
  EXPECT_EQ(classifyInput(L, G, find(*F, "inv")), InputKind::Free);
  // A load whose only user is in the group becomes a vector load.
  EXPECT_EQ(classifyInput(L, G, find(*F, "ld")), InputKind::Free);
  // A load with a user outside the group does not.
  EXPECT_EQ(classifyInput(L, G, find(*F, "ld2")), InputKind::GprToVec);
}

TEST_F(SAVIOClassifyInputTest, ExtractElement) {
  Function *F = parse(LoopIR);
  ASSERT_TRUE(F);
  Group G = groupOf({find(*F, "g3"), find(*F, "g4")});
  // %vec is an argument, so in the loop these extracts are invariant; classify
  // outside a loop to test the lane rule itself.
  EXPECT_EQ(classifyInput(nullptr, G, find(*F, "e2")), InputKind::LaneToLane0);
  EXPECT_EQ(classifyInput(nullptr, G, find(*F, "evar")), InputKind::GprToVec);
  EXPECT_EQ(classifyInput(nullptr, G, find(*F, "eoob")), InputKind::GprToVec);
}

TEST_F(SAVIOClassifyInputTest, InvariantWithoutPreheader) {
  Function *F = parse(R"(
    define void @f(i32 %a, i1 %k, i64 %n) {
    entry:
      %inv = add i32 %a, 1
      br i1 %k, label %loop, label %mid
    mid:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ 0, %mid ], [ %i.next, %loop ]
      %g = add i32 %inv, %a
      %i.next = add i64 %i, 1
      %c = icmp ult i64 %i.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  Loop *L = LI.getLoopFor(find(*F, "g")->getParent());
  ASSERT_TRUE(L);
  ASSERT_FALSE(L->getLoopPreheader());
  Group G = groupOf({find(*F, "g")});
  // The transfer stays in the loop.
  EXPECT_EQ(classifyInput(L, G, find(*F, "inv")), InputKind::GprToVec);
  EXPECT_EQ(classifyInput(L, G, F->getArg(0)), InputKind::GprToVec);
}

} // namespace
