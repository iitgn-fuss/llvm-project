//===- RegionCost.cpp - SAVIO region cost model ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/RegionCost.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <cassert>

#define DEBUG_TYPE "savio"

using namespace llvm;
using namespace llvm::savio;

double RegionEstimate::t() const { return std::max({TRes, TRec, TDisp, TMem}); }

Bound RegionEstimate::limiting() const {
  double T = t();
  if (TRec == T)
    return Bound::Recurrence;
  if (TRes == T)
    return Bound::Resource;
  if (TDisp == T)
    return Bound::Dispatch;
  return Bound::Memory;
}

namespace {

/// A directed edge in the loop dependence graph.
///
/// An edge from \c From to \c To with carried distance \c Dist indicates that
/// the value produced by \c From on iteration $i$ is consumed by \c To on
/// iteration $i + \text{Dist}$.
/// - For intra-iteration dependencies (within the same loop iteration), \c Dist == 0.
/// - For loop-carried dependencies (e.g. cross-iteration PHIs), \c Dist >= 1.
struct Edge {
  unsigned From;
  unsigned To;
  unsigned Dist;
};

/// Looks for a dependence cycle whose latency-to-distance ratio strictly
/// exceeds \p Lambda, using Bellman-Ford longest-path relaxation.
///
/// In loop pipelining, the recurrence bound is defined by the maximum cycle ratio:
///   T_rec = max_C [ sum_{v in C} Latency(v) / sum_{e in C} Distance(e) ]
///
/// For a trial throughput candidate Lambda, a cycle C violates the bound if:
///   sum_{v in C} Latency(v) / sum_{e in C} Distance(e) > Lambda
///   <=> sum_{e in C} [ Latency(From(e)) - Lambda * Distance(e) ] > 0
///
/// By assigning each edge e = (From, To) the weight:
///   weight(e) = Latency(From) - Lambda * Dist(e)
/// the problem reduces to detecting a strictly positive-weight cycle in the
/// dependence graph.
///
/// If Bellman-Ford can still relax an edge after \c NumNodes rounds, a positive
/// cycle is guaranteed to exist. Walking backward \c NumNodes steps along the
/// relaxation parent pointers lands inside the cycle, which is then traversed
/// to compute its exact cycle ratio Latency / Distance (> Lambda).
/// Returns the new cycle ratio, or 0.0 if no cycle exceeds Lambda.
double betterCycleRatio(ArrayRef<CostNode> Nodes, ArrayRef<Edge> Edges,
                        double Lambda) {
  constexpr double Eps = 1e-9;
  unsigned NumNodes = Nodes.size();
  SmallVector<double, 32> LongestDist(NumNodes, 0.0);
  SmallVector<int, 32> ParentEdge(NumNodes, -1); // Index in Edges that set LongestDist.
  int LastRelaxedNode = -1;

  for (unsigned Round = 0; Round != NumNodes + 1; ++Round) {
    LastRelaxedNode = -1;
    for (unsigned EdgeIdx = 0; EdgeIdx != Edges.size(); ++EdgeIdx) {
      const Edge &E = Edges[EdgeIdx];
      double Weight = Nodes[E.From].Cost.Latency - Lambda * E.Dist;
      if (LongestDist[E.From] + Weight > LongestDist[E.To] + Eps) {
        LongestDist[E.To] = LongestDist[E.From] + Weight;
        ParentEdge[E.To] = EdgeIdx;
        LastRelaxedNode = E.To;
      }
    }
    // If no edge was relaxed in this round, all paths have settled and no
    // positive-weight cycle exists.
    if (LastRelaxedNode < 0)
      return 0.0;
  }

  // Still relaxing after NumNodes rounds: a positive cycle exists.
  // Walk backward NumNodes steps along parent pointers to ensure landing inside the cycle.
  unsigned CycleEntryNode = LastRelaxedNode;
  for (unsigned I = 0; I != NumNodes; ++I) {
    assert(ParentEdge[CycleEntryNode] >= 0 &&
           "a node on a positive cycle must have a parent edge");
    CycleEntryNode = Edges[ParentEdge[CycleEntryNode]].From;
  }

  // Trace the cycle from CycleEntryNode back to itself and sum its latencies and distances.
  double CycleLatency = 0.0;
  unsigned CycleDistance = 0;
  unsigned CurrNode = CycleEntryNode;
  do {
    const Edge &E = Edges[ParentEdge[CurrNode]];
    CycleLatency += Nodes[E.From].Cost.Latency;
    CycleDistance += E.Dist;
    CurrNode = E.From;
  } while (CurrNode != CycleEntryNode);

  return CycleDistance ? CycleLatency / CycleDistance : 0.0;
}

/// Computes the exact recurrence bound T_rec: the maximum cycle ratio over
/// all cycles in the loop dependence graph:
///   T_rec = max_C [ sum_{v in C} Latency(v) / sum_{e in C} Distance(e) ]
///
/// Implements Howard's / Lawler's cycle ratio iteration algorithm:
/// Starting from Lambda = 0, each invocation of \c betterCycleRatio either
/// finds a cycle that beats Lambda and raises Lambda to that cycle's exact ratio,
/// or determines that no better cycle exists. Because the graph has finitely
/// many simple cycles, the iteration terminates quickly with the exact maximum.
double maxCycleRatio(ArrayRef<CostNode> Nodes) {
  SmallVector<Edge, 64> Edges;
  for (unsigned ConsumerIdx = 0; ConsumerIdx != Nodes.size(); ++ConsumerIdx) {
    for (unsigned PredIdx : Nodes[ConsumerIdx].Preds)
      Edges.push_back({PredIdx, ConsumerIdx, 0});
    for (auto [ProducerIdx, Dist] : Nodes[ConsumerIdx].Carried)
      Edges.push_back({ProducerIdx, ConsumerIdx, Dist});
  }

  double Lambda = 0.0;
  while (double BetterRatio = betterCycleRatio(Nodes, Edges, Lambda)) {
    if (BetterRatio <= Lambda)
      break;
    Lambda = BetterRatio;
  }
  return Lambda;
}

/// Computes TRes for targets with execution port sets.
///
/// When execution units are partitioned into discrete execution ports, an
/// instruction of class C can typically issue to any port in its allowable
/// port set S_C.
/// By generalized max-flow min-cut (Hall's condition), the bottleneck for any
/// subset of execution ports U is given by the aggregate load of all classes
/// whose allowable ports form a subset of U, divided by the number of ports in U:
///   T(U) = sum_{C : S_C <= U} Load[C] / |U|
///
/// The true resource bound TRes is the maximum over all non-empty port unions U:
///   TRes = max_{U} T(U)
///
/// Classes with no assigned port set operate as independent resource pools
/// (Load[C] / Units[C]).
///
/// The loader limits the number of distinct port sets to at most 16, so the
/// number of evaluated subsets is at most 2^16 = 65536.
void resourceBoundOverPorts(const PortDescription &PD, ArrayRef<double> Load,
                            RegionEstimate &E) {
  SmallVector<std::pair<uint64_t, unsigned>, 16> ClassPortSets; // (port mask, class)
  for (unsigned ClassIdx = 0; ClassIdx != PD.numClasses(); ++ClassIdx) {
    if (Load[ClassIdx] <= 0)
      continue;
    ArrayRef<unsigned> Ports = PD.ports(ClassIdx, E.With512);
    if (Ports.empty()) {
      // Independent resource pool without port assignments.
      double ClassTime = Load[ClassIdx] / PD.units(ClassIdx);
      if (ClassTime > E.TRes) {
        E.TRes = ClassTime;
        E.BusiestClass = ClassIdx;
      }
      continue;
    }
    uint64_t PortMask = 0;
    for (unsigned Port : Ports)
      PortMask |= uint64_t(1) << Port;
    ClassPortSets.push_back({PortMask, ClassIdx});
  }

  // Identify distinct port sets. Every possible port union is a union of a
  // subset of these distinct masks.
  SmallVector<uint64_t, 16> DistinctPortMasks;
  for (auto &[PortMask, ClassIdx] : ClassPortSets) {
    if (!is_contained(DistinctPortMasks, PortMask))
      DistinctPortMasks.push_back(PortMask);
  }

  // Enumerate all 2^k - 1 non-empty subsets of distinct port sets.
  for (uint32_t SubsetIdx = 1;
       SubsetIdx < (uint32_t(1) << DistinctPortMasks.size());
       ++SubsetIdx) {
    uint64_t PortUnionMask = 0;
    for (unsigned I = 0; I != DistinctPortMasks.size(); ++I) {
      if ((SubsetIdx >> I) & 1)
        PortUnionMask |= DistinctPortMasks[I];
    }

    double UnionClassLoad = 0.0;
    double MaxClassLoadInUnion = 0.0;
    unsigned BusiestClassInUnion = 0;

    for (auto &[PortMask, ClassIdx] : ClassPortSets) {
      // Check if ClassIdx's port mask is entirely contained within PortUnionMask.
      if (PortMask & ~PortUnionMask)
        continue;
      UnionClassLoad += Load[ClassIdx];
      if (Load[ClassIdx] > MaxClassLoadInUnion) {
        MaxClassLoadInUnion = Load[ClassIdx];
        BusiestClassInUnion = ClassIdx;
      }
    }

    double SubsetThroughputTime =
        UnionClassLoad / llvm::popcount(PortUnionMask);
    if (SubsetThroughputTime > E.TRes) {
      E.TRes = SubsetThroughputTime;
      E.BusiestClass = BusiestClassInUnion;
    }
  }
}

} // namespace

double savio::effectiveConcurrency(unsigned NK, double UopsPerIter, unsigned W,
                                   bool IsLoop) {
  if (!IsLoop)
    return NK;
  double Overlap = 1.0;
  if (W && UopsPerIter > 0)
    Overlap = std::max(1.0, double(W) / UopsPerIter);
  return std::min(8.0, NK * Overlap);
}

void savio::applyConcurrency(SmallVectorImpl<CostNode> &Nodes,
                             const PortDescription &PD, bool IsLoop) {
  DenseMap<const ConcurrencyTable *, unsigned> Count;
  double Uops = 0;
  for (const CostNode &N : Nodes) {
    if (N.Conc)
      ++Count[N.Conc];
    Uops += N.Cost.Uops;
  }
  if (Count.empty())
    return;
  for (CostNode &N : Nodes)
    if (N.Conc && !N.Cost.Uses.empty())
      N.Cost.Uses[0].Cycles = N.Conc->at(
          effectiveConcurrency(Count[N.Conc], Uops, PD.windowUops(), IsLoop));
}

RegionEstimate savio::estimateRegion(ArrayRef<CostNode> Nodes,
                                     const PortDescription &PD, bool IsLoop) {
  RegionEstimate Est;

#ifndef NDEBUG
  // Verify topological order and edge validity contract.
  for (unsigned I = 0; I != Nodes.size(); ++I) {
    for (unsigned P : Nodes[I].Preds)
      assert(P < I && "nodes must be in topological order");
    for (auto [Producer, Dist] : Nodes[I].Carried)
      assert(Producer < Nodes.size() && Dist >= 1 && "bad carried edge");
  }
#endif

  // Phase 1: Aggregate resource class occupancy loads and total micro-ops.
  SmallVector<double, 8> ClassLoads(PD.numClasses(), 0.0);
  double TotalUops = 0.0;
  for (const CostNode &Node : Nodes) {
    for (const ResourceUse &Usage : Node.Cost.Uses)
      ClassLoads[Usage.Class] += Usage.Cycles;
    TotalUops += Node.Cost.Uops;
  }

  LLVM_DEBUG({
    dbgs() << "savio: class loads (cycles, whole region):";
    for (unsigned C = 0; C != PD.numClasses(); ++C)
      if (ClassLoads[C] > 0)
        dbgs() << ' ' << PD.className(C) << '=' << ClassLoads[C];
    dbgs() << " uops=" << TotalUops << '\n';
  });

  // Phase 2: Compute Resource Bound (TRes).
  if (!PD.hasPortSets()) {
    // Independent resource units per class.
    for (unsigned C = 0; C != PD.numClasses(); ++C) {
      double ClassTime = ClassLoads[C] / PD.units(C);
      if (ClassTime > Est.TRes) {
        Est.TRes = ClassTime;
        Est.BusiestClass = C;
      }
    }
  } else {
    // Shared execution port sets.
    Est.With512 = any_of(Nodes, [](const CostNode &N) { return N.Wide512; });
    resourceBoundOverPorts(PD, ClassLoads, Est);
  }

  // Phase 3: Compute Front-End Dispatch Bound (TDisp).
  Est.TDisp = TotalUops / PD.dispatchWidth();

  // Phase 4: Compute Recurrence / Critical Path Bound (TRec).
  // In a loop, TRec is the maximum cycle ratio over every cycle of the
  // dependence graph, regardless of how many carried edges it crosses.
  if (IsLoop) {
    Est.TRec = maxCycleRatio(Nodes);
  } else if (PD.criticalPathOutsideLoops()) {
    // Outside loops, calculate the longest latency path in the DAG.
    // (A target description can disable this via 'critical_path_outside_loops'
    // when neighboring code overlaps arithmetic latency).
    SmallVector<double, 16> CriticalDist(Nodes.size(), 0.0);
    for (unsigned I = 0; I != Nodes.size(); ++I) {
      double MaxPredLatency = 0.0;
      for (unsigned Pred : Nodes[I].Preds)
        MaxPredLatency = std::max(MaxPredLatency, CriticalDist[Pred]);
      CriticalDist[I] = MaxPredLatency + Nodes[I].Cost.Latency;
      Est.TRec = std::max(Est.TRec, CriticalDist[I]);
    }
  }

  return Est;
}
