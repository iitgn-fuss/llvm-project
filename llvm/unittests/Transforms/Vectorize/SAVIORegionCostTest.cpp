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

/// test2s1vc: window_uops 12; sv.udiv v4i32 is a 4-uop sequence using 8 vector
/// cycles, with cycles per instance 8, 6, 4 at m = 1, 2, 4.
class SAVIOConcurrencyTest : public testing::Test {
protected:
  void SetUp() override {
    SmallString<128> Root(SAVIO_TEST_INPUTS);
    sys::path::append(Root, "port-info");
    auto PDOrErr = PortDescription::load(Root, "AArch64", "test2s1vc");
    ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
    PD = std::move(*PDOrErr);
  }
  CostNode node(StringRef Key) {
    CostNode N{*PD->irOpCost(Key), {}, {}};
    N.Conc = PD->concurrency(Key);
    return N;
  }
  /// Per-instance vector cycles of the sequence nodes after the pass.
  double seqCycles(unsigned NSeq, unsigned NAdd, bool IsLoop) {
    SmallVector<CostNode> N;
    for (unsigned I = 0; I != NSeq; ++I)
      N.push_back(node("sv.udiv v4i32"));
    for (unsigned I = 0; I != NAdd; ++I)
      N.push_back(node("add i32"));
    applyConcurrency(N, *PD, IsLoop);
    return N[0].Cost.Uses[0].Cycles;
  }
  std::unique_ptr<PortDescription> PD;
};

TEST_F(SAVIOConcurrencyTest, EffectiveConcurrency) {
  // Loop: min(8, n * max(1, W / uops)); outside a loop: n.
  EXPECT_DOUBLE_EQ(effectiveConcurrency(2, 12, 12, true), 2.0);
  EXPECT_DOUBLE_EQ(effectiveConcurrency(2, 8, 12, true), 3.0);
  EXPECT_DOUBLE_EQ(effectiveConcurrency(2, 100, 12, true), 2.0);  // max(1, .)
  EXPECT_DOUBLE_EQ(effectiveConcurrency(3, 1, 12, true), 8.0);    // capped
  EXPECT_DOUBLE_EQ(effectiveConcurrency(3, 1, 12, false), 3.0);
  EXPECT_DOUBLE_EQ(effectiveConcurrency(2, 8, 0, true), 2.0);     // no window
}

TEST_F(SAVIOConcurrencyTest, LoopUsesWindow) {
  // 2 sequence nodes (4 uops each) + 4 adds = 12 uops: m_eff = 2 -> 6.
  EXPECT_DOUBLE_EQ(seqCycles(2, 4, true), 6.0);
  // 2 sequence nodes alone = 8 uops: m_eff = 3 -> 5 (interpolated).
  EXPECT_DOUBLE_EQ(seqCycles(2, 0, true), 5.0);
  // 1 sequence node alone = 4 uops: m_eff = 3 -> 5.
  EXPECT_DOUBLE_EQ(seqCycles(1, 0, true), 5.0);
}

TEST_F(SAVIOConcurrencyTest, StraightLineUsesCount) {
  EXPECT_DOUBLE_EQ(seqCycles(2, 0, false), 6.0);
  EXPECT_DOUBLE_EQ(seqCycles(2, 40, false), 6.0);  // uops do not matter
  EXPECT_DOUBLE_EQ(seqCycles(1, 0, false), 8.0);
  EXPECT_DOUBLE_EQ(seqCycles(9, 0, false), 4.0);   // clamped to the table
}

TEST_F(SAVIOConcurrencyTest, EntryWithoutTableUnchanged) {
  SmallVector<CostNode> N;
  N.push_back(node("sv.add v4i32"));
  N.push_back(node("add i32"));
  double Before = N[0].Cost.Uses[0].Cycles;
  applyConcurrency(N, *PD, true);
  EXPECT_DOUBLE_EQ(N[0].Cost.Uses[0].Cycles, Before);
  EXPECT_EQ(N[0].Cost.Uops, 1u);
}

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

TEST_F(SAVIORegionCostTest, OverlappingCyclesNeedSeveralSteps) {
  // A chain 0 -> 1 -> 2 -> 3 of latency 9 each. Cycle P closes through all four
  // nodes at distance 4 (36 / 4 = 9) and has the larger total latency, which
  // the first search at lambda 0 finds; cycle Q, nodes 0 and 1 at distance 1
  // (18 / 1 = 18), overlaps it and beats it, so a second step is needed.
  SmallVector<CostNode> N;
  add(N, "udiv i32", 4);
  for (unsigned I = 1; I != 4; ++I)
    N[I].Preds.push_back(I - 1);
  N[0].Carried.push_back({3, 4}); // P
  N[0].Carried.push_back({1, 1}); // Q
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

namespace {

/// The port-info of the Emerald Rapids x86 machine (a copy of
/// src/port-info/x86/emeraldrapids in the lit fixtures): classes list their
/// issue ports, vec_alu {0,1,5} ({0,5} with a 512-bit op), scalar_alu
/// {0,1,5,6,10}, load {2,3,11}, store {4,9}, int_div has none.
class SAVIOPortSetTest : public testing::Test {
protected:
  void SetUp() override {
    SmallString<128> Root(SAVIO_TEST_INPUTS);
    sys::path::append(Root, "..", "X86", "Inputs", "port-info");
    auto PDOrErr = PortDescription::load(Root, "x86", "emeraldrapids");
    ASSERT_TRUE(!!PDOrErr) << toString(PDOrErr.takeError());
    PD = std::move(*PDOrErr);
  }

  unsigned cls(StringRef Name) {
    for (unsigned C = 0; C != PD->numClasses(); ++C)
      if (PD->className(C) == Name)
        return C;
    ADD_FAILURE() << "no class " << Name.str();
    return 0;
  }

  /// A node of no latency and no micro-ops that puts \p Cycles on \p Class.
  CostNode load(StringRef Class, double Cycles, bool Wide512 = false) {
    CostNode N;
    N.Cost = OpCost{{{cls(Class), Cycles}}, 0, 0};
    N.Wide512 = Wide512;
    return N;
  }

  std::unique_ptr<PortDescription> PD;
};

TEST_F(SAVIOPortSetTest, LoadsPortSets) {
  EXPECT_TRUE(PD->hasPortSets());
  EXPECT_EQ(PD->ports(cls("vec_alu"), false), ArrayRef<unsigned>({0, 1, 5}));
  EXPECT_EQ(PD->ports(cls("vec_alu"), true), ArrayRef<unsigned>({0, 5}));
  // vec_mul moves p0,p1 to p0,p5: not a subset.
  EXPECT_EQ(PD->ports(cls("vec_mul"), false), ArrayRef<unsigned>({0, 1}));
  EXPECT_EQ(PD->ports(cls("vec_mul"), true), ArrayRef<unsigned>({0, 5}));
  // No ports_with_512: the same set; no ports at all: empty.
  EXPECT_EQ(PD->ports(cls("scalar_alu"), true),
            ArrayRef<unsigned>({0, 1, 5, 6, 10}));
  EXPECT_TRUE(PD->ports(cls("int_div"), false).empty());
  EXPECT_TRUE(PD->ports(cls("int_div"), true).empty());
}

TEST_F(SAVIOPortSetTest, DisjointSetsAreToday) {
  // alu {0,1,5,6,10} 10, load {2,3,11} 6, store {4,9} 4, divider 2 (no ports):
  // every class alone: 2.0, 2.0, 2.0, 2.0.
  SmallVector<CostNode> N = {load("scalar_alu", 10), load("load", 6),
                             load("store", 4), load("int_div", 2)};
  RegionEstimate E = estimateRegion(N, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 2.0);
  EXPECT_FALSE(E.With512);
}

TEST_F(SAVIOPortSetTest, SharedPorts) {
  // scalar_alu load 5 + vec_alu load 3 on the union {0,1,5,6,10}: 8 / 5.
  SmallVector<CostNode> N = {load("scalar_alu", 5), load("vec_alu", 3)};
  RegionEstimate E = estimateRegion(N, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 1.6);
  EXPECT_EQ(PD->className(E.BusiestClass), "scalar_alu");
  // vec_alu alone: 3 / 3.
  SmallVector<CostNode> V = {load("vec_alu", 3)};
  E = estimateRegion(V, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 1.0);
  EXPECT_EQ(PD->className(E.BusiestClass), "vec_alu");
}

TEST_F(SAVIOPortSetTest, With512IsRegionWide) {
  // vec_alu load 3 with a 512-bit op in the region: {0,5}.
  SmallVector<CostNode> N = {load("vec_alu", 3, /*Wide512=*/true)};
  RegionEstimate E = estimateRegion(N, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 1.5);
  EXPECT_TRUE(E.With512);
  // The 512-bit op may be another node: vec_alu keeps load 3, one node 512.
  SmallVector<CostNode> M = {load("vec_alu", 3), load("vec_shift", 0, true)};
  E = estimateRegion(M, *PD, true);
  EXPECT_DOUBLE_EQ(E.TRes, 1.5);
  EXPECT_TRUE(E.With512);
}

// The rows below were computed by scripts/savio/validate_x86_ports.py from the
// committed emeraldrapids port-info (TRes of its cost(), the loads and the
// 512-bit flag printed for the same atoms counts as the pair CSV row).
TEST_F(SAVIOPortSetTest, AgreesWithValidateX86Ports) {
  struct Row {
    const char *Name;
    SmallVector<std::pair<const char *, double>, 3> Loads;
    bool Wide;
    double TRes;
  } Rows[] = {
      // alu+va256 (22 + 12 atoms): 34.22 / 5 on {0,1,5,6,10}.
      {"alu+va256", {{"scalar_alu", 22.22}, {"vec_alu", 12.0}}, false, 6.844},
      // va256+va512 (12 + 8): 20 / |{0,5}|.
      {"va256+va512", {{"vec_alu", 20.0}}, true, 10.0},
      // shlx+vsh512 (8 + 4): shifts on {0,6} and vec_shift on {0}: 12.16 / 2
      // is beaten by (8.16 + 4) / 2 = 6.08 over {0,6}.
      {"shlx+vsh512", {{"scalar_shift", 8.16}, {"vec_shift", 4.0}}, true, 6.08},
      // va256+cvt256 (12 + 4): vec_alu, vec_cvt, vec_cvt_shuffle on {0,1,5}.
      {"va256+cvt256",
       {{"vec_alu", 12.0}, {"vec_cvt", 4.0}, {"vec_cvt_shuffle", 4.0}},
       false,
       6.666666666666667},
  };
  for (const Row &R : Rows) {
    SmallVector<CostNode> N;
    for (auto [Class, Cycles] : R.Loads)
      N.push_back(load(Class, Cycles, R.Wide));
    RegionEstimate E = estimateRegion(N, *PD, true);
    EXPECT_NEAR(E.TRes, R.TRes, 1e-9) << R.Name;
    EXPECT_EQ(E.With512, R.Wide) << R.Name;
  }
}

} // namespace
