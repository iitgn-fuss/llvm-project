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

TEST_F(SAVIOClassifyInputTest, RegionGraphPhiOfPhi) {
  // %a takes the latch value of %b, which takes %x: %x reaches %a's user (%x
  // itself) two iterations later.
  Function *F = parse(R"(
    define i32 @f(i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %a = phi i32 [ 1, %entry ], [ %b, %loop ]
      %b = phi i32 [ 2, %entry ], [ %x, %loop ]
      %x = add i32 %a, 3
      %i.next = add i64 %i, 1
      %c = icmp ult i64 %i.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret i32 %x
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  BasicBlock *BB = find(*F, "x")->getParent();
  Loop *L = LI.getLoopFor(BB);
  ASSERT_TRUE(L);
  RegionGraph G = buildRegionGraph(*BB, L);
  unsigned X = G.Index.lookup(find(*F, "x"));
  ASSERT_EQ(G.Carried[X].size(), 1u);
  EXPECT_EQ(G.Carried[X][0], std::make_pair(X, 2u));
  // %i.next reads %i one iteration back; the branch is a node in a loop.
  unsigned IN = G.Index.lookup(find(*F, "i.next"));
  ASSERT_EQ(G.Carried[IN].size(), 1u);
  EXPECT_EQ(G.Carried[IN][0], std::make_pair(IN, 1u));
  EXPECT_EQ(G.Insts.back(), BB->getTerminator());
}

TEST_F(SAVIOClassifyInputTest, RegionGraphNoTerminatorOutsideLoop) {
  Function *F = parse(R"(
    define i32 @f(i32 %a) {
      %g = add i32 %a, 7
      ret i32 %g
    })");
  ASSERT_TRUE(F);
  RegionGraph G = buildRegionGraph(F->getEntryBlock(), nullptr);
  ASSERT_EQ(G.Insts.size(), 1u);
  EXPECT_EQ(G.Insts[0], find(*F, "g"));
}

TEST_F(SAVIOClassifyInputTest, SliceClassEqualForCopies) {
  // Four copies of the bucket feeder (mul, and, add, urem) off the pinned
  // fshl/xor accumulator chain: four groups, one class.
  Function *F = parse(R"(
    declare i32 @llvm.fshl.i32(i32, i32, i32)
    define i32 @f(ptr %b, ptr %c, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %acc = phi i32 [ 0, %entry ], [ %a3, %loop ]
      %pb0 = getelementptr inbounds i32, ptr %b, i64 0
      %pc0 = getelementptr inbounds i32, ptr %c, i64 0
      %b0 = load i32, ptr %pb0
      %c0 = load i32, ptr %pc0
      %m0 = mul i32 %b0, 7
      %k0 = and i32 %c0, 1023
      %d0 = add i32 %k0, 1
      %r0 = urem i32 %m0, %d0
      %pb1 = getelementptr inbounds i32, ptr %b, i64 1
      %pc1 = getelementptr inbounds i32, ptr %c, i64 1
      %b1 = load i32, ptr %pb1
      %c1 = load i32, ptr %pc1
      %m1 = mul i32 %b1, 7
      %k1 = and i32 %c1, 1023
      %d1 = add i32 %k1, 1
      %r1 = urem i32 %m1, %d1
      %pb2 = getelementptr inbounds i32, ptr %b, i64 2
      %pc2 = getelementptr inbounds i32, ptr %c, i64 2
      %b2 = load i32, ptr %pb2
      %c2 = load i32, ptr %pc2
      %m2 = mul i32 %b2, 7
      %k2 = and i32 %c2, 1023
      %d2 = add i32 %k2, 1
      %r2 = urem i32 %m2, %d2
      %pb3 = getelementptr inbounds i32, ptr %b, i64 3
      %pc3 = getelementptr inbounds i32, ptr %c, i64 3
      %b3 = load i32, ptr %pb3
      %c3 = load i32, ptr %pc3
      %m3 = mul i32 %b3, 7
      %k3 = and i32 %c3, 1023
      %d3 = add i32 %k3, 1
      %r3 = urem i32 %m3, %d3
      %rot0 = call i32 @llvm.fshl.i32(i32 %acc, i32 %acc, i32 5)
      %a0 = xor i32 %rot0, %r0
      %rot1 = call i32 @llvm.fshl.i32(i32 %a0, i32 %a0, i32 5)
      %a1 = xor i32 %rot1, %r1
      %rot2 = call i32 @llvm.fshl.i32(i32 %a1, i32 %a1, i32 5)
      %a2 = xor i32 %rot2, %r2
      %rot3 = call i32 @llvm.fshl.i32(i32 %a2, i32 %a2, i32 5)
      %a3 = xor i32 %rot3, %r3
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %a3
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 4u);
  for (const Group &G : R.Groups) {
    EXPECT_FALSE(G.Recurrence);
    EXPECT_EQ(G.Ops.size(), 4u);
    EXPECT_EQ(G.SliceClass, R.Groups[0].SliceClass);
  }
}

TEST_F(SAVIOClassifyInputTest, SliceClassDiffersByOpcodeOrConstant) {
  // The third copy masks with 511 instead of 1023.
  Function *F = parse(R"(
    declare i32 @llvm.fshl.i32(i32, i32, i32)
    define i32 @f(ptr %b, ptr %c, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %acc = phi i32 [ 0, %entry ], [ %a3, %loop ]
      %pb0 = getelementptr inbounds i32, ptr %b, i64 0
      %pc0 = getelementptr inbounds i32, ptr %c, i64 0
      %b0 = load i32, ptr %pb0
      %c0 = load i32, ptr %pc0
      %m0 = mul i32 %b0, 7
      %k0 = and i32 %c0, 1023
      %d0 = add i32 %k0, 1
      %r0 = urem i32 %m0, %d0
      %pb1 = getelementptr inbounds i32, ptr %b, i64 1
      %pc1 = getelementptr inbounds i32, ptr %c, i64 1
      %b1 = load i32, ptr %pb1
      %c1 = load i32, ptr %pc1
      %m1 = mul i32 %b1, 7
      %k1 = and i32 %c1, 1023
      %d1 = add i32 %k1, 1
      %r1 = urem i32 %m1, %d1
      %pb2 = getelementptr inbounds i32, ptr %b, i64 2
      %pc2 = getelementptr inbounds i32, ptr %c, i64 2
      %b2 = load i32, ptr %pb2
      %c2 = load i32, ptr %pc2
      %m2 = mul i32 %b2, 7
      %k2 = and i32 %c2, 511
      %d2 = add i32 %k2, 1
      %r2 = urem i32 %m2, %d2
      %pb3 = getelementptr inbounds i32, ptr %b, i64 3
      %pc3 = getelementptr inbounds i32, ptr %c, i64 3
      %b3 = load i32, ptr %pb3
      %c3 = load i32, ptr %pc3
      %m3 = mul i32 %b3, 7
      %k3 = and i32 %c3, 1023
      %d3 = add i32 %k3, 1
      %r3 = urem i32 %m3, %d3
      %rot0 = call i32 @llvm.fshl.i32(i32 %acc, i32 %acc, i32 5)
      %a0 = xor i32 %rot0, %r0
      %rot1 = call i32 @llvm.fshl.i32(i32 %a0, i32 %a0, i32 5)
      %a1 = xor i32 %rot1, %r1
      %rot2 = call i32 @llvm.fshl.i32(i32 %a1, i32 %a1, i32 5)
      %a2 = xor i32 %rot2, %r2
      %rot3 = call i32 @llvm.fshl.i32(i32 %a2, i32 %a2, i32 5)
      %a3 = xor i32 %rot3, %r3
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %a3
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 4u);
  EXPECT_EQ(R.Groups[0].SliceClass, R.Groups[1].SliceClass);
  EXPECT_EQ(R.Groups[0].SliceClass, R.Groups[3].SliceClass);
  EXPECT_NE(R.Groups[0].SliceClass, R.Groups[2].SliceClass);
}

} // namespace
