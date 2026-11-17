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
#include "llvm/Support/Path.h"
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

TEST_F(SAVIOClassifyInputTest, SliceClassDiffersByOpcode) {
  // Same shape, but the second slice divides instead of taking the remainder.
  Function *F = parse(R"(
    define void @f(ptr %o, i32 %a, i32 %b) {
      %x = urem i32 %a, %b
      %y = udiv i32 %a, %b
      store i32 %x, ptr %o
      %q = getelementptr i32, ptr %o, i64 1
      store i32 %y, ptr %q
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  ASSERT_EQ(Regions[0].Groups.size(), 2u);
  EXPECT_NE(Regions[0].Groups[0].SliceClass, Regions[0].Groups[1].SliceClass);
}

TEST_F(SAVIOClassifyInputTest, SliceClassInputIdentity) {
  // mul x,x differs from mul x,y; mul y,y equals mul x,x.
  Function *F = parse(R"(
    define void @f(ptr %o, i32 %x, i32 %y) {
      %g0 = mul i32 %x, %x
      %g1 = mul i32 %x, %y
      %g2 = mul i32 %y, %y
      store i32 %g0, ptr %o
      %q1 = getelementptr i32, ptr %o, i64 1
      store i32 %g1, ptr %q1
      %q2 = getelementptr i32, ptr %o, i64 2
      store i32 %g2, ptr %q2
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 3u);
  EXPECT_NE(R.Groups[0].SliceClass, R.Groups[1].SliceClass);
  EXPECT_EQ(R.Groups[0].SliceClass, R.Groups[2].SliceClass);
}

TEST_F(SAVIOClassifyInputTest, SliceClassDiffersAcrossRegions) {
  // Two identical loops: their slices are not instances of each other.
  Function *F = parse(R"(
    define void @f(ptr %b, ptr %c, i64 %n) {
    entry:
      br label %l1
    l1:
      %i = phi i64 [ 0, %entry ], [ %i.next, %l1 ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %v = load i32, ptr %pb
      %m = mul i32 %v, 3
      store i32 %m, ptr %pb
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %l1, label %mid
    mid:
      br label %l2
    l2:
      %j = phi i64 [ 0, %mid ], [ %j.next, %l2 ]
      %pc = getelementptr inbounds i32, ptr %c, i64 %j
      %w = load i32, ptr %pc
      %k = mul i32 %w, 3
      store i32 %k, ptr %pc
      %j.next = add nuw i64 %j, 1
      %cmp2 = icmp ult i64 %j.next, %n
      br i1 %cmp2, label %l2, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 2u);
  ASSERT_EQ(Regions[0].Groups.size(), 1u);
  ASSERT_EQ(Regions[1].Groups.size(), 1u);
  EXPECT_NE(Regions[0].Groups[0].SliceClass, Regions[1].Groups[0].SliceClass);
}

TEST_F(SAVIOClassifyInputTest, RecurrenceAllInScope) {
  // h = (h ^ v) * 5: every member in scope, one phi: one Recurrence group.
  Function *F = parse(R"(
    declare i32 @llvm.fshl.i32(i32, i32, i32)
    define i32 @f(ptr %b, ptr %p, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %h = phi i32 [ 1, %entry ], [ %hn, %loop ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %v = load i32, ptr %pb
      %x = xor i32 %h, %v
      %hn = mul i32 %x, 5
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %hn
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  ASSERT_EQ(Regions[0].Groups.size(), 1u);
  const Group &G = Regions[0].Groups[0];
  EXPECT_TRUE(G.Recurrence);
  EXPECT_EQ(G.Ops.size(), 2u);
  ASSERT_EQ(G.CarriedPhis.size(), 1u);
  EXPECT_EQ(G.CarriedPhis[0]->getName(), "h");
}

TEST_F(SAVIOClassifyInputTest, RecurrenceThroughPhiOfPhiDropped) {
  // a takes b's latch value, b takes x: x reads a two iterations back. The
  // phi of a phi is not joined, so the recurrence is not a group.
  Function *F = parse(R"(
    declare i32 @llvm.fshl.i32(i32, i32, i32)
    define i32 @f(ptr %b, ptr %p, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %a = phi i32 [ 1, %entry ], [ %b2, %loop ]
      %b2 = phi i32 [ 2, %entry ], [ %x, %loop ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %v = load i32, ptr %pb
      %x = add i32 %a, %v
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %x
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  EXPECT_TRUE(Regions.empty());
}

TEST_F(SAVIOClassifyInputTest, RecurrenceWithOutsideReaderOfPhiDropped) {
  // The phi h is read by the recurrence and also by a store outside it: the
  // recurrence is dropped (conservative v1 choice), no group at all.
  Function *F = parse(R"(
    declare i32 @llvm.fshl.i32(i32, i32, i32)
    define i32 @f(ptr %b, ptr %p, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %h = phi i32 [ 1, %entry ], [ %hn, %loop ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %v = load i32, ptr %pb
      store i32 %h, ptr %p
      %x = xor i32 %h, %v
      %hn = mul i32 %x, 5
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %hn
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  EXPECT_TRUE(Regions.empty());
}

TEST_F(SAVIOClassifyInputTest, RecurrenceWithOutOfScopeMemberNextToSlice) {
  // acc = fshl(acc ^ (v * 7), 13) * 5: the recurrence has an out-of-scope
  // member (fshl), so none of its operations is a candidate; the feeder v * 7
  // is a slice off the cycle and survives.
  Function *F = parse(R"(
    declare i32 @llvm.fshl.i32(i32, i32, i32)
    define i32 @f(ptr %b, ptr %p, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %acc = phi i32 [ 0, %entry ], [ %an, %loop ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %v = load i32, ptr %pb
      %m = mul i32 %v, 7
      %s = xor i32 %acc, %m
      %r = call i32 @llvm.fshl.i32(i32 %s, i32 %s, i32 13)
      %an = mul i32 %r, 5
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %an
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  ASSERT_EQ(Regions[0].Groups.size(), 1u);
  const Group &G = Regions[0].Groups[0];
  EXPECT_FALSE(G.Recurrence);
  EXPECT_TRUE(G.CarriedPhis.empty());
  ASSERT_EQ(G.Ops.size(), 1u);
  EXPECT_EQ(G.Ops[0]->getName(), "m");
}

TEST_F(SAVIOClassifyInputTest, PromotedLatchOnNonJoinedPhiCostsTransfers) {
  // acc = fshl(acc, 5) ^ v with the xor Singleton Vectorized by hand and its
  // phi not joined: GprToVec (rot) and VecToGpr (xor) lie on the cycle, so T
  // goes from fshl + xor = 2 to 1 + 1 + 1 + 1 = 4 (the Part 1 numbers).
  Function *F = parse(R"(
    declare i32 @llvm.fshl.i32(i32, i32, i32)
    define i32 @f(ptr %b, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %acc = phi i32 [ 0, %entry ], [ %x, %loop ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %v = load i32, ptr %pb
      %rot = call i32 @llvm.fshl.i32(i32 %acc, i32 %acc, i32 5)
      %x = xor i32 %rot, %v
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %x
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  BasicBlock *BB = find(*F, "x")->getParent();
  Loop *L = LI.getLoopFor(BB);
  ASSERT_TRUE(L);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test2s1v-wide");
  ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
  Region R;
  R.BB = BB;
  R.L = L;
  Group G;
  Instruction *X = find(*F, "x");
  G.Ops.push_back(X);
  G.Inputs.push_back(&X->getOperandUse(0));
  G.Inputs.push_back(&X->getOperandUse(1));
  G.LiveOuts.push_back(X);
  R.Groups.push_back(G);
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  bool Saved = SAVIOForce;
  SAVIOForce = true;
  Decision D = decide(R, **PDOrErr, Fallback, 0.05);
  SAVIOForce = Saved;
  ASSERT_EQ(D.Promoted.size(), 1u);
  EXPECT_DOUBLE_EQ(D.Before.t(), 2.0);
  EXPECT_DOUBLE_EQ(D.After.t(), 4.0);
  EXPECT_EQ(D.After.limiting(), Bound::Recurrence);
}

TEST_F(SAVIOClassifyInputTest, FractionPicksBalance) {
  // Five instances of one slice (load, add 1, store). Machine test1s1v-frac:
  // one scalar unit (add 2 cycles), one vector unit (add 3 cycles), free
  // transfers. T(k) = max(2(5-k), 3k): 10, 8, 6, 9, 12, 15 -> k = 2.
  Function *F = parse(R"(
    define void @f(ptr %b, ptr %o, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %pb0 = getelementptr inbounds i32, ptr %b, i64 0
      %b0 = load i32, ptr %pb0
      %r0 = add i32 %b0, 1
      %po0 = getelementptr inbounds i32, ptr %o, i64 0
      store i32 %r0, ptr %po0
      %pb1 = getelementptr inbounds i32, ptr %b, i64 1
      %b1 = load i32, ptr %pb1
      %r1 = add i32 %b1, 1
      %po1 = getelementptr inbounds i32, ptr %o, i64 1
      store i32 %r1, ptr %po1
      %pb2 = getelementptr inbounds i32, ptr %b, i64 2
      %b2 = load i32, ptr %pb2
      %r2 = add i32 %b2, 1
      %po2 = getelementptr inbounds i32, ptr %o, i64 2
      store i32 %r2, ptr %po2
      %pb3 = getelementptr inbounds i32, ptr %b, i64 3
      %b3 = load i32, ptr %pb3
      %r3 = add i32 %b3, 1
      %po3 = getelementptr inbounds i32, ptr %o, i64 3
      store i32 %r3, ptr %po3
      %pb4 = getelementptr inbounds i32, ptr %b, i64 4
      %b4 = load i32, ptr %pb4
      %r4 = add i32 %b4, 1
      %po4 = getelementptr inbounds i32, ptr %o, i64 4
      store i32 %r4, ptr %po4
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 5u);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test1s1v-frac");
  ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  Decision D = decide(R, **PDOrErr, Fallback, 0.05);
  ASSERT_EQ(D.MovedPerClass.size(), 1u);
  EXPECT_EQ(D.MovedPerClass[0].Class, R.Groups[0].SliceClass);
  EXPECT_EQ(D.MovedPerClass[0].K, 2u);
  EXPECT_EQ(D.MovedPerClass[0].U, 5u);
  EXPECT_EQ(D.Promoted.size(), 2u);
  EXPECT_EQ(D.Unroll, 1u);
  EXPECT_DOUBLE_EQ(D.Before.t(), 10.0);
  EXPECT_DOUBLE_EQ(D.After.t(), 6.0);
}

TEST_F(SAVIOClassifyInputTest, FractionNonMonotone) {
  // Four instances of one slice outside a loop (add 1, store). Machine
  // test1s1v-seq: scalar add 2 cycles on one unit; the sv.add sequence costs
  // 10 cycles alone but 3 per instance from two in flight. T(k) = max(2(4-k),
  // vec(k)) = 8, 10, 6, 9, 12: one instance loses to the unchanged block, two
  // win. A greedy that adds one group at a time stops at k = 0; the search
  // finds k = 2, spread evenly over indices 0 and 2.
  Function *F = parse(R"(
    define void @f(ptr %o, i32 %a0, i32 %a1, i32 %a2, i32 %a3, i32 %s) {
    entry:
      %p0 = getelementptr inbounds i32, ptr %o, i64 0
      %r0 = add i32 %a0, 1
      store i32 %r0, ptr %p0
      %p1 = getelementptr inbounds i32, ptr %o, i64 1
      %r1 = add i32 %a1, 1
      store i32 %r1, ptr %p1
      %p2 = getelementptr inbounds i32, ptr %o, i64 2
      %r2 = add i32 %a2, 1
      store i32 %r2, ptr %p2
      %p3 = getelementptr inbounds i32, ptr %o, i64 3
      %r3 = add i32 %a3, 1
      store i32 %r3, ptr %p3
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 4u);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test1s1v-seq");
  ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  Decision D = decide(R, **PDOrErr, Fallback, 0.05);
  ASSERT_EQ(D.MovedPerClass.size(), 1u);
  EXPECT_EQ(D.MovedPerClass[0].K, 2u);
  EXPECT_EQ(D.MovedPerClass[0].U, 4u);
  ASSERT_EQ(D.Promoted.size(), 2u);
  EXPECT_EQ(D.Promoted[0], 0u);
  EXPECT_EQ(D.Promoted[1], 2u);
  EXPECT_DOUBLE_EQ(D.Before.t(), 8.0);
  EXPECT_DOUBLE_EQ(D.After.t(), 6.0);
}

TEST_F(SAVIOClassifyInputTest, FractionTwoClasses) {
  // The four adds of FractionNonMonotone plus one sub (scalar 4 cycles, vector
  // 1). Unchanged T = 12. Coordinate descent: class add first, k = 2 (T 8;
  // k = 1: 10, k = 3: 9); then class sub, k = 1 (T 7). The greedy loop moves
  // the sub first (T 8) and then no single add helps, ending at T 8.
  Function *F = parse(R"(
    define void @f(ptr %o, i32 %a0, i32 %a1, i32 %a2, i32 %a3, i32 %s) {
    entry:
      %p0 = getelementptr inbounds i32, ptr %o, i64 0
      %r0 = add i32 %a0, 1
      store i32 %r0, ptr %p0
      %p1 = getelementptr inbounds i32, ptr %o, i64 1
      %r1 = add i32 %a1, 1
      store i32 %r1, ptr %p1
      %p2 = getelementptr inbounds i32, ptr %o, i64 2
      %r2 = add i32 %a2, 1
      store i32 %r2, ptr %p2
      %p3 = getelementptr inbounds i32, ptr %o, i64 3
      %r3 = add i32 %a3, 1
      store i32 %r3, ptr %p3
      %ps = getelementptr inbounds i32, ptr %o, i64 4
      %rs = sub i32 %s, 1
      store i32 %rs, ptr %ps
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 5u);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test1s1v-seq");
  ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  Decision D = decide(R, **PDOrErr, Fallback, 0.05);
  ASSERT_EQ(D.MovedPerClass.size(), 2u);
  EXPECT_NE(D.MovedPerClass[0].Class, D.MovedPerClass[1].Class);
  EXPECT_EQ(D.MovedPerClass[0].K, 2u);
  EXPECT_EQ(D.MovedPerClass[0].U, 4u);
  EXPECT_EQ(D.MovedPerClass[1].K, 1u);
  EXPECT_EQ(D.MovedPerClass[1].U, 1u);
  ASSERT_EQ(D.Promoted.size(), 3u);
  EXPECT_EQ(D.Promoted[0], 0u);
  EXPECT_EQ(D.Promoted[1], 2u);
  EXPECT_EQ(D.Promoted[2], 4u);
  EXPECT_DOUBLE_EQ(D.Before.t(), 12.0);
  EXPECT_DOUBLE_EQ(D.After.t(), 7.0);
}

TEST_F(SAVIOClassifyInputTest, IsomorphicRecurrencesHaveOwnClasses) {
  // Two independent copies of h = (h ^ v) * 5 in one block: each is a whole
  // recurrence, a single instance (k in {0,1}), so the classes differ.
  Function *F = parse(R"(
    define i32 @f(ptr %b, ptr %c, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %h = phi i32 [ 1, %entry ], [ %hn, %loop ]
      %g = phi i32 [ 1, %entry ], [ %gn, %loop ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %pc = getelementptr inbounds i32, ptr %c, i64 %i
      %v = load i32, ptr %pb
      %w = load i32, ptr %pc
      %x = xor i32 %h, %v
      %hn = mul i32 %x, 5
      %y = xor i32 %g, %w
      %gn = mul i32 %y, 5
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      %s = or i32 %hn, %gn
      ret i32 %s
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_GE(Regions.size(), 1u);
  const Region &LR = Regions[0];
  ASSERT_EQ(LR.Groups.size(), 2u);
  EXPECT_TRUE(LR.Groups[0].Recurrence);
  EXPECT_TRUE(LR.Groups[1].Recurrence);
  EXPECT_NE(LR.Groups[0].SliceClass, LR.Groups[1].SliceClass);
}

TEST_F(SAVIOClassifyInputTest, VirtualUnrollKeepsRecurrenceOneInstance) {
  // One slice instance (load, add 1, store) next to a distance-1 recurrence
  // acc = acc * 3 whose value is used after the loop. Machine test1s1v-frac:
  // scalar add 2 cycles on the one scalar unit, vector add 3 on the one vector
  // unit, scalar and vector mul 1, free transfers. Without unrolling the
  // recurrence moves (T 3 -> 2: scalar add 2, vector mul 1). Unrolled by 5 the
  // slice has 5 instances while the recurrence stays ONE chain of 5 muls: with
  // 3 of 5 adds on the vector unit and the chain on the scalar unit, scalar
  // busy 2*2 + 5 = 9, vector 3*3 = 9, T = 9 per 5 iterations = 1.8 per original
  // iteration, more than the margin better than the 2.0 without unrolling.
  Function *F = parse(R"(
    define i32 @f(ptr %b, ptr %o, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %acc = phi i32 [ 1, %entry ], [ %accn, %loop ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %v = load i32, ptr %pb
      %r = add i32 %v, 1
      %po = getelementptr inbounds i32, ptr %o, i64 %i
      store i32 %r, ptr %po
      %accn = mul i32 %acc, 3
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %accn
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 2u);
  unsigned SliceIdx = R.Groups[0].Recurrence ? 1 : 0;
  ASSERT_FALSE(R.Groups[SliceIdx].Recurrence);
  ASSERT_TRUE(R.Groups[1 - SliceIdx].Recurrence);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test1s1v-frac");
  ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  Decision Plain = decide(R, **PDOrErr, Fallback, 0.05);
  EXPECT_EQ(Plain.Unroll, 1u);
  EXPECT_DOUBLE_EQ(Plain.Before.t(), 3.0);
  EXPECT_DOUBLE_EQ(Plain.After.t(), 2.0);

  Decision D = decide(R, **PDOrErr, Fallback, 0.05, /*CanUnroll=*/true);
  EXPECT_EQ(D.Unroll, 5u);
  EXPECT_TRUE(D.Promoted.empty());
  EXPECT_DOUBLE_EQ(D.Before.t(), 3.0);
  EXPECT_DOUBLE_EQ(D.After.t(), 1.8); // Per original iteration.
  // The slice moves 3 of its 5 instances; the recurrence is never counted as
  // more than one instance.
  ASSERT_EQ(D.MovedPerClass.size(), 1u);
  EXPECT_EQ(D.MovedPerClass[0].Class, R.Groups[SliceIdx].SliceClass);
  EXPECT_EQ(D.MovedPerClass[0].K, 3u);
  EXPECT_EQ(D.MovedPerClass[0].U, 5u);
  EXPECT_NE(D.MovedPerClass[0].Class, R.Groups[1 - SliceIdx].SliceClass);
}

TEST_F(SAVIOClassifyInputTest, VirtualUnrollMustBeatNotUnrolling) {
  // Same loop and machine as VirtualUnrollKeepsRecurrenceOneInstance: T per
  // original iteration is 3.0 unchanged and 2.0 with the recurrence moved and
  // no unrolling (the assignment already in hand); unrolled by 2, 3, 4 or 6 it
  // is 2.0 too, by 5 it is 1.8, i.e. 10% better than 2.0. With margin 0.105 the
  // unrolled-by-5 candidate beats the unchanged 3.0 by the margin and is the
  // best, and the assignment in hand (2.0) is outside best * 1.105 = 1.989, so
  // a selection that only compares with the unchanged loop picks the unroll
  // by 5 for a 10% gain, below the 10.5% margin. Unrolling must beat not
  // unrolling by the margin: the loop stays as it is.
  Function *F = parse(R"(
    define i32 @f(ptr %b, ptr %o, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %acc = phi i32 [ 1, %entry ], [ %accn, %loop ]
      %pb = getelementptr inbounds i32, ptr %b, i64 %i
      %v = load i32, ptr %pb
      %r = add i32 %v, 1
      %po = getelementptr inbounds i32, ptr %o, i64 %i
      store i32 %r, ptr %po
      %accn = mul i32 %acc, 3
      %i.next = add nuw i64 %i, 1
      %cmp = icmp ult i64 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret i32 %accn
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 2u);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test1s1v-frac");
  ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  Decision Plain = decide(R, **PDOrErr, Fallback, 0.105);
  EXPECT_EQ(Plain.Unroll, 1u);
  EXPECT_DOUBLE_EQ(Plain.After.t(), 2.0);
  Decision D = decide(R, **PDOrErr, Fallback, 0.105, /*CanUnroll=*/true);
  EXPECT_EQ(D.Unroll, 1u);
  EXPECT_DOUBLE_EQ(D.After.t(), 2.0);
  EXPECT_FALSE(D.Promoted.empty());
}

// Scalarize: slices of vector candidates, and what the cost model charges at
// their boundaries (machine test3scv: scalar 2 units, vector 1, mem 1, every
// scalar and vector op 1 cycle, load/store 1 mem cycle, lane extract and insert
// 3 vector cycles each; dispatch width 64, so the resource bound decides).

TEST_F(SAVIOClassifyInputTest, ScalarizeSliceOwnsLoadsAndStores) {
  // Two copies of load, mul, xor, store: two instances of one ToScalar class.
  // A scalar i32 chain beside them is its own ToVector group, never merged.
  Function *F = parse(R"(
    define void @f(ptr %k, ptr %o, ptr %s, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %pk0 = getelementptr inbounds i32, ptr %k, i64 %i
      %w0 = load <4 x i32>, ptr %pk0
      %m0 = mul <4 x i32> %w0, splat (i32 3)
      %x0 = xor <4 x i32> %m0, %w0
      %po0 = getelementptr inbounds i32, ptr %o, i64 %i
      store <4 x i32> %x0, ptr %po0
      %pk1 = getelementptr inbounds i32, ptr %k, i64 8
      %w1 = load <4 x i32>, ptr %pk1
      %m1 = mul <4 x i32> %w1, splat (i32 3)
      %x1 = xor <4 x i32> %m1, %w1
      %po1 = getelementptr inbounds i32, ptr %o, i64 8
      store <4 x i32> %x1, ptr %po1
      %sl = load i32, ptr %s
      %sm = mul i32 %sl, 3
      store i32 %sm, ptr %s
      %i.next = add nuw i64 %i, 4
      %c = icmp ult i64 %i.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  const Region &R = Regions[0];
  ASSERT_EQ(R.Groups.size(), 3u);
  EXPECT_EQ(R.Groups[0].Dir, Direction::ToScalar);
  EXPECT_EQ(R.Groups[1].Dir, Direction::ToScalar);
  EXPECT_EQ(R.Groups[2].Dir, Direction::ToVector);
  EXPECT_EQ(R.Groups[0].SliceClass, R.Groups[1].SliceClass);
  EXPECT_NE(R.Groups[0].SliceClass, R.Groups[2].SliceClass);
  ASSERT_EQ(R.Groups[0].Ops.size(), 2u);
  ASSERT_EQ(R.Groups[0].OwnedLoads.size(), 1u);
  EXPECT_EQ(R.Groups[0].OwnedLoads[0]->getName(), "w0");
  ASSERT_EQ(R.Groups[0].OwnedStores.size(), 1u);
  EXPECT_FALSE(liveOutNeedsInsert(R.Groups[0], *R.Groups[0].Ops[1]));
}

TEST_F(SAVIOClassifyInputTest, ScalarizeSliceNotMixedAndDivideExcluded) {
  // A vector udiv is not a Scalarize candidate; a lane extracted from a vector
  // chain feeding a scalar op stops both slices.
  Function *F = parse(R"(
    define void @f(<4 x i32> %a, <4 x i32> %b, ptr %o) {
      %v = add <4 x i32> %a, %b
      %d = udiv <4 x i32> %v, %b
      %e = extractelement <4 x i32> %d, i32 1
      %s = add i32 %e, 1
      store i32 %s, ptr %o
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  unsigned Vec = 0, Scal = 0, Div = 0;
  for (const Group &G : Regions[0].Groups) {
    if (G.VectorDivide)
      ++Div;
    else if (G.Dir == Direction::ToScalar)
      ++Vec;
    else
      ++Scal;
  }
  EXPECT_EQ(Vec, 1u);  // %v
  EXPECT_EQ(Scal, 1u); // %s
  EXPECT_EQ(Div, 2u);  // the two lane pairs of %d
}

/// Forces every group of the only region of \p IR and returns the decision.
/// The machine is test3scv.
static Decision decideForcedOn(Function *F, std::unique_ptr<PortDescription> &PD,
                               Region &Out, SmallVector<Region, 4> &Regions,
                               LoopInfo &LI) {
  Regions = formRegions(*F, LI);
  EXPECT_EQ(Regions.size(), 1u);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test3scv");
  EXPECT_TRUE(!!PDOrErr);
  PD = std::move(*PDOrErr);
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  bool Saved = SAVIOForce;
  SAVIOForce = true;
  Decision D = decide(Regions[0], *PD, Fallback, 0.05);
  SAVIOForce = Saved;
  return D;
}

TEST_F(SAVIOClassifyInputTest, ScalarizeCostOwnsLoadsAndStores) {
  // load, add, store, all owned: as vector 1 + 1 mem cycles (T 2, mem); as
  // scalars 4 loads + 4 stores = 8 mem cycles, 4 adds on 2 units = 2, and no
  // lane transfer (each would add 3 vector cycles and lift T above 8).
  Function *F = parse(R"(
    define void @f(ptr %k, ptr %o, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %pk = getelementptr inbounds i32, ptr %k, i64 %i
      %w = load <4 x i32>, ptr %pk
      %r = add <4 x i32> %w, splat (i32 1)
      %po = getelementptr inbounds i32, ptr %o, i64 %i
      store <4 x i32> %r, ptr %po
      %i.next = add nuw i64 %i, 4
      %c = icmp ult i64 %i.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  std::unique_ptr<PortDescription> PD;
  Region Dummy;
  SmallVector<Region, 4> Regions;
  Decision D = decideForcedOn(F, PD, Dummy, Regions, LI);
  ASSERT_EQ(D.Promoted.size(), 1u);
  EXPECT_DOUBLE_EQ(D.Before.t(), 2.0);
  EXPECT_DOUBLE_EQ(D.After.t(), 8.0);
  EXPECT_EQ(PD->className(D.After.BusiestClass), "mem");
}

TEST_F(SAVIOClassifyInputTest, ScalarizeCostSharedLoad) {
  // The load is also stored whole elsewhere, so the slice does not own it:
  // the vector load stays (1 mem cycle) and four lane extracts (4 x 3 vector
  // cycles) feed the lanes; the owned store is 4 scalar stores (mem 1 + 1 + 4).
  Function *F = parse(R"(
    define void @f(ptr %k, ptr %o, ptr %q, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %pk = getelementptr inbounds i32, ptr %k, i64 %i
      %w = load <4 x i32>, ptr %pk
      %r = add <4 x i32> %w, splat (i32 1)
      %po = getelementptr inbounds i32, ptr %o, i64 %i
      store <4 x i32> %r, ptr %po
      store <4 x i32> %w, ptr %q
      %i.next = add nuw i64 %i, 4
      %c = icmp ult i64 %i.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  std::unique_ptr<PortDescription> PD;
  Region Dummy;
  SmallVector<Region, 4> Regions;
  ASSERT_EQ(formRegions(*F, LI)[0].Groups[0].OwnedLoads.size(), 0u);
  Decision D = decideForcedOn(F, PD, Dummy, Regions, LI);
  ASSERT_EQ(D.Promoted.size(), 1u);
  EXPECT_DOUBLE_EQ(D.After.t(), 12.0);
  EXPECT_EQ(PD->className(D.After.BusiestClass), "vector");
}

TEST_F(SAVIOClassifyInputTest, ScalarizeCostLiveOut) {
  // The result is used by a call, not only stored: four lane inserts (4 x 3
  // vector cycles) bring it back; the load is owned (4 mem cycles).
  Function *F = parse(R"(
    declare void @use(<4 x i32>)
    define void @f(ptr %k, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %pk = getelementptr inbounds i32, ptr %k, i64 %i
      %w = load <4 x i32>, ptr %pk
      %r = add <4 x i32> %w, splat (i32 1)
      call void @use(<4 x i32> %r)
      %i.next = add nuw i64 %i, 4
      %c = icmp ult i64 %i.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  std::unique_ptr<PortDescription> PD;
  Region Dummy;
  SmallVector<Region, 4> Regions;
  Decision D = decideForcedOn(F, PD, Dummy, Regions, LI);
  ASSERT_EQ(D.Promoted.size(), 1u);
  EXPECT_TRUE(liveOutNeedsInsert(Regions[0].Groups[0],
                                 *Regions[0].Groups[0].Ops[0]));
  EXPECT_DOUBLE_EQ(D.After.t(), 12.0);
  EXPECT_EQ(PD->className(D.After.BusiestClass), "vector");
}

TEST_F(SAVIOClassifyInputTest, ScalarizeCostFusedShift) {
  // Five `x = xor (lshr y, C), y` pairs: the shift is used once, by the xor, so
  // the scalar lanes cost one fused xor.lshr.imm each (1 cycle, 4 lanes on 2
  // units = 2 per pair, T 10) and not an lshr plus an xor (T 20).
  Function *F = parse(R"(
    define void @f(ptr %k, ptr %o, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %pk = getelementptr inbounds i32, ptr %k, i64 %i
      %w = load <4 x i32>, ptr %pk
      %s0 = lshr <4 x i32> %w, splat (i32 16)
      %x0 = xor <4 x i32> %s0, %w
      %s1 = lshr <4 x i32> %x0, splat (i32 13)
      %x1 = xor <4 x i32> %s1, %x0
      %s2 = lshr <4 x i32> %x1, splat (i32 7)
      %x2 = xor <4 x i32> %s2, %x1
      %s3 = lshr <4 x i32> %x2, splat (i32 5)
      %x3 = xor <4 x i32> %s3, %x2
      %s4 = lshr <4 x i32> %x3, splat (i32 3)
      %x4 = xor <4 x i32> %s4, %x3
      %po = getelementptr inbounds i32, ptr %o, i64 %i
      store <4 x i32> %x4, ptr %po
      %i.next = add nuw i64 %i, 4
      %c = icmp ult i64 %i.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  std::unique_ptr<PortDescription> PD;
  Region Dummy;
  SmallVector<Region, 4> Regions;
  Decision D = decideForcedOn(F, PD, Dummy, Regions, LI);
  ASSERT_EQ(D.Promoted.size(), 1u);
  EXPECT_DOUBLE_EQ(D.After.t(), 10.0);
  EXPECT_EQ(PD->className(D.After.BusiestClass), "scalar");
}

TEST_F(SAVIOClassifyInputTest, ScalarizeMissingKeyIsNotCostable) {
  // test2s1v (Part-2 format) has no vector, lane or load keys: forcing leaves
  // the vector slice alone.
  Function *F = parse(R"(
    define void @f(ptr %k, ptr %o, i64 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
      %pk = getelementptr inbounds i32, ptr %k, i64 %i
      %w = load <4 x i32>, ptr %pk
      %r = add <4 x i32> %w, splat (i32 1)
      %po = getelementptr inbounds i32, ptr %o, i64 %i
      store <4 x i32> %r, ptr %po
      %i.next = add nuw i64 %i, 4
      %c = icmp ult i64 %i.next, %n
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    })");
  ASSERT_TRUE(F);
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  SmallVector<Region, 4> Regions = formRegions(*F, LI);
  ASSERT_EQ(Regions.size(), 1u);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test2s1v");
  ASSERT_TRUE(!!PDOrErr);
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  bool Saved = SAVIOForce;
  SAVIOForce = true;
  Decision D = decide(Regions[0], **PDOrErr, Fallback, 0.05);
  SAVIOForce = Saved;
  EXPECT_TRUE(D.Promoted.empty());
}

} // namespace
