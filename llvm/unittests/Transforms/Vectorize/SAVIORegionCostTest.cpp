//===- SAVIORegionCostTest.cpp - SAVIO region cost model tests -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/RegionCost.h"
#include "llvm/Support/Path.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::savio;

namespace {

/// Fixture machine test2s1v: scalar 2 units, vector 1, int_div 1, fp_div 1,
/// dispatch width 4.
class SAVIORegionCostTest : public testing::Test {
protected:
  void SetUp() override {
    SmallString<128> Root(SAVIO_TEST_INPUTS);
    sys::path::append(Root, "port-info");
    auto PDOrErr = PortDescription::load(Root, "AArch64", "test2s1v");
    ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
    PD = std::move(*PDOrErr);
  }

  CostNode node(StringRef Key) {
    std::optional<OpCost> C = PD->irOpCost(Key);
    EXPECT_TRUE(C.has_value()) << Key.str();
    return CostNode{C.value_or(OpCost{{}, 0, 0}), {}, {}};
  }

  CostNode transfer(TransferKind K) {
    std::optional<OpCost> C = PD->transferCost(K);
    EXPECT_TRUE(C.has_value());
    return CostNode{C.value_or(OpCost{{}, 0, 0}), {}, {}};
  }

  void add(SmallVectorImpl<CostNode> &V, StringRef Key, unsigned N) {
    for (unsigned I = 0; I != N; ++I)
      V.push_back(node(Key));
  }

  std::unique_ptr<PortDescription> PD;
};

TEST_F(SAVIORegionCostTest, ScalarHeavy) {
  SmallVector<CostNode> N;
  add(N, "add i32", 6);
  RegionEstimate E = estimateRegion(N, *PD, /*IsLoop=*/true);
  EXPECT_DOUBLE_EQ(E.TRes, 3.0);
  EXPECT_DOUBLE_EQ(E.TDisp, 1.5);
  EXPECT_DOUBLE_EQ(E.t(), 3.0);
  EXPECT_EQ(E.limiting(), Bound::Resource);
  EXPECT_EQ(PD->className(E.BusiestClass), "scalar");
}

TEST_F(SAVIORegionCostTest, ScalarHeavySplit) {
  SmallVector<CostNode> N;
  add(N, "add i32", 4);
  add(N, "sv.add v4i32", 2);
  RegionEstimate E = estimateRegion(N, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 2.0);
  EXPECT_DOUBLE_EQ(E.t(), 2.0);
}

TEST_F(SAVIORegionCostTest, TransfersDominate) {
  SmallVector<CostNode> N;
  add(N, "add i32", 4);
  add(N, "sv.add v4i32", 2);
  for (unsigned I = 0; I != 3; ++I) {
    N.push_back(transfer(TransferKind::GprToVec));
    N.push_back(transfer(TransferKind::VecToGpr));
  }
  RegionEstimate E = estimateRegion(N, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 8.0);
  EXPECT_DOUBLE_EQ(E.TDisp, 3.0);
  EXPECT_DOUBLE_EQ(E.t(), 8.0);
  EXPECT_EQ(PD->className(E.BusiestClass), "vector");
}

TEST_F(SAVIORegionCostTest, DivideLoop) {
  SmallVector<CostNode> Int;
  add(Int, "udiv i32", 4);
  EXPECT_DOUBLE_EQ(estimateRegion(Int, *PD, true).t(), 16.0);

  // Two scalar divides on int_div plus two lane-0 vector divides, which load
  // fp_div (4 cycles each) and vector (3 cycles each).
  SmallVector<CostNode> Mixed;
  add(Mixed, "udiv i32", 2);
  add(Mixed, "sv.udiv v4i32", 2);
  RegionEstimate E = estimateRegion(Mixed, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 8.0);
  EXPECT_DOUBLE_EQ(E.t(), 8.0);
}

TEST_F(SAVIORegionCostTest, DivideChain) {
  SmallVector<CostNode> N;
  add(N, "udiv i32", 4);
  for (unsigned I = 1; I != 4; ++I)
    N[I].Preds.push_back(I - 1);
  N[0].Carried.push_back({3, 1}); // last -> first, distance 1
  RegionEstimate E = estimateRegion(N, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRec, 36.0); // 4 divides of latency 9
  EXPECT_DOUBLE_EQ(E.TRes, 16.0);
  EXPECT_EQ(E.limiting(), Bound::Recurrence);

  // Distance 2 halves the bound.
  N[0].Carried[0].second = 2;
  EXPECT_DOUBLE_EQ(estimateRegion(N, *PD, true).TRec, 18.0);

  // Outside a loop carried edges are ignored; the longest path is the chain.
  N[0].Carried.clear();
  EXPECT_DOUBLE_EQ(estimateRegion(N, *PD, false).TRec, 36.0);
}

TEST_F(SAVIORegionCostTest, RecurrenceSkipsUnrelatedNodes) {
  // Nodes 0 -> 1 form the cycle (latency 9 + 9); node 2 is a downstream
  // node that must not enter the cycle's path.
  SmallVector<CostNode> N;
  add(N, "udiv i32", 3);
  N[1].Preds.push_back(0);
  N[2].Preds.push_back(1);
  N[0].Carried.push_back({1, 1});
  EXPECT_DOUBLE_EQ(estimateRegion(N, *PD, true).TRec, 18.0);
}

TEST_F(SAVIORegionCostTest, MultiPhiCycle) {
  // a' = f(b), b' = g(a): two phis. A (latency 3) and B (latency 5) each take
  // the other's value from the previous iteration; the cycle holds two
  // distances, so the bound is (3 + 5) / 2, not 5 (the largest per-edge path).
  SmallVector<CostNode> N;
  N.push_back(CostNode{OpCost{{}, 3, 0}, {}, {}});
  N.push_back(CostNode{OpCost{{}, 5, 0}, {}, {}});
  N[0].Carried.push_back({1, 1}); // A reads B of the previous iteration.
  N[1].Carried.push_back({0, 1}); // B reads A of the previous iteration.
  RegionEstimate E = estimateRegion(N, *PD, /*IsLoop=*/true);
  EXPECT_DOUBLE_EQ(E.TRec, 4.0);
  EXPECT_EQ(E.limiting(), Bound::Recurrence);
}

TEST_F(SAVIORegionCostTest, UncostedNodeKeepsChain) {
  // X has latency 2 and no resource use (a fallback-costed instruction); the
  // cycle X -> Y -> X still counts its latency.
  SmallVector<CostNode> N;
  N.push_back(CostNode{OpCost{{}, 2, 0}, {}, {}});
  N.push_back(node("add i32"));
  N[1].Preds.push_back(0);
  N[0].Carried.push_back({1, 1});
  EXPECT_DOUBLE_EQ(estimateRegion(N, *PD, true).TRec, 3.0);
}

TEST_F(SAVIORegionCostTest, MaxRatioPicksWorstCycle) {
  // Two disjoint cycles: latency 18 at distance 1 (ratio 18) and latency 36 at
  // distance 3 (ratio 12). The larger ratio is the bound.
  SmallVector<CostNode> N;
  add(N, "udiv i32", 6);
  N[1].Preds.push_back(0);
  N[0].Carried.push_back({1, 1}); // cycle 0,1: 18 / 1
  N[3].Preds.push_back(2);
  N[4].Preds.push_back(3);
  N[5].Preds.push_back(4);
  N[2].Carried.push_back({5, 3}); // cycle 2..5: 36 / 3 = 12
  EXPECT_DOUBLE_EQ(estimateRegion(N, *PD, true).TRec, 18.0);
}

TEST_F(SAVIORegionCostTest, LimitingTies) {
  // Single scalar add: TRes 0.5, TDisp 0.25, TRec 1 (latency) outside a loop.
  SmallVector<CostNode> N;
  add(N, "add i32", 1);
  EXPECT_EQ(estimateRegion(N, *PD, false).limiting(), Bound::Recurrence);

  // Hand-built costs (the fixture has no such op): TRes == TDisp == 1.
  unsigned Scalar = 0;
  for (unsigned C = 0; C != PD->numClasses(); ++C)
    if (PD->className(C) == "scalar")
      Scalar = C;
  OpCost C{{{Scalar, 2.0}}, 0, 4};
  SmallVector<CostNode> T;
  T.push_back(CostNode{C, {}, {}});
  RegionEstimate E = estimateRegion(T, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 1.0);
  EXPECT_DOUBLE_EQ(E.TDisp, 1.0);
  EXPECT_EQ(E.limiting(), Bound::Resource);
}

TEST_F(SAVIORegionCostTest, InvariantOperandFree) {
  // A node whose only input is a loop-invariant value has no transfer node;
  // estimateRegion charges only the nodes it is given.
  SmallVector<CostNode> N;
  add(N, "urem i32", 4);
  RegionEstimate E = estimateRegion(N, *PD, true);
  EXPECT_DOUBLE_EQ(E.t(), 16.0);
  EXPECT_EQ(PD->className(E.BusiestClass), "int_div");
}

} // end namespace
