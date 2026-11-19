//===- SAVIOGroupingTest.cpp - SAVIO region and grouping tests ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SAVIOTestUtils.h"
#include "llvm/Transforms/Vectorize/SAVIO/SearchCore.h"

using namespace llvm;
using namespace llvm::savio;

namespace {
using SAVIOGroupingTest = SAVIOClassifyInputTest;

TEST_F(SAVIOGroupingTest, CSearchCorePairSearch) {
  const unsigned Sizes[] = {3, 2};
  const unsigned Recurrence[] = {0, 0};
  unsigned Moved[] = {0, 0};
  double Best = savio_search_fraction(
      2, Sizes, Recurrence, Moved, 5.0, 0.0, nullptr,
      searchCoreTestEstimate, 4);
  EXPECT_DOUBLE_EQ(Best, 0.0);
  EXPECT_EQ(Moved[0], 2u);
  EXPECT_EQ(Moved[1], 1u);
}

TEST_F(SAVIOGroupingTest, CSearchCoreRecurrenceAndCallbackCount) {
  const unsigned Sizes[] = {8};
  const unsigned Recurrence[] = {1};
  unsigned Moved[] = {0};
  SearchCoreCounter Counter{0};
  double Best = savio_search_fraction(
      1, Sizes, Recurrence, Moved, 2.0, 0.0, &Counter,
      searchCoreCounterEstimate, 4);
  EXPECT_DOUBLE_EQ(Best, 1.0);
  EXPECT_EQ(Moved[0], 1u);
  EXPECT_EQ(Counter.Calls, 2u);
}

TEST_F(SAVIOGroupingTest, CSearchCoreNullEstimateIsNoOp) {
  const unsigned Sizes[] = {4};
  unsigned Moved[] = {0};
  double Best = savio_search_fraction(
      1, Sizes, nullptr, Moved, 3.25, 0.05, nullptr, nullptr, 4);
  EXPECT_DOUBLE_EQ(Best, 3.25);
  EXPECT_EQ(Moved[0], 0u);
}

TEST_F(SAVIOGroupingTest, Constant) {
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

TEST_F(SAVIOGroupingTest, ArgumentOutsideLoop) {
  Function *F = parse(R"(
    define i32 @f(i32 %a, i32 %b) {
      %g = add i32 %a, %b
      ret i32 %g
    })");
  ASSERT_TRUE(F);
  Group G = groupOf({find(*F, "g")});
  EXPECT_EQ(classifyInput(nullptr, G, F->getArg(0)), InputKind::GprToVec);
}

TEST_F(SAVIOGroupingTest, InLoopWithPreheader) {
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

TEST_F(SAVIOGroupingTest, ExtractElement) {
  Function *F = parse(LoopIR);
  ASSERT_TRUE(F);
  Group G = groupOf({find(*F, "g3"), find(*F, "g4")});
  // %vec is an argument, so in the loop these extracts are invariant; classify
  // outside a loop to test the lane rule itself.
  EXPECT_EQ(classifyInput(nullptr, G, find(*F, "e2")), InputKind::LaneToLane0);
  EXPECT_EQ(classifyInput(nullptr, G, find(*F, "evar")), InputKind::GprToVec);
  EXPECT_EQ(classifyInput(nullptr, G, find(*F, "eoob")), InputKind::GprToVec);
}

TEST_F(SAVIOGroupingTest, InvariantWithoutPreheader) {
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

TEST_F(SAVIOGroupingTest, RegionGraphPhiOfPhi) {
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

TEST_F(SAVIOGroupingTest, RegionGraphNoTerminatorOutsideLoop) {
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

TEST_F(SAVIOGroupingTest, SliceClassEqualForCopies) {
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

TEST_F(SAVIOGroupingTest, SliceClassDiffersByOpcodeOrConstant) {
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

TEST_F(SAVIOGroupingTest, SliceClassDiffersByOpcode) {
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

TEST_F(SAVIOGroupingTest, SliceClassInputIdentity) {
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

TEST_F(SAVIOGroupingTest, SliceClassDiffersAcrossRegions) {
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

TEST_F(SAVIOGroupingTest, RecurrenceAllInScope) {
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

TEST_F(SAVIOGroupingTest, RecurrenceThroughPhiOfPhiDropped) {
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

TEST_F(SAVIOGroupingTest, RecurrenceWithOutsideReaderOfPhiDropped) {
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

TEST_F(SAVIOGroupingTest, RecurrenceWithOutOfScopeMemberNextToSlice) {
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

TEST_F(SAVIOGroupingTest, PromotedLatchOnNonJoinedPhiCostsTransfers) {
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


} // namespace
