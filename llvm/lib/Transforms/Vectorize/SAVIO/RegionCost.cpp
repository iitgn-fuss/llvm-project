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

/// An edge of the dependence graph of a loop: the value of node From, Dist
/// iterations earlier, is an operand of node To. Its weight is the latency of
/// From, so a cycle's weight is the sum of the latencies of its nodes.
struct Edge {
  unsigned From, To, Dist;
};

/// Looks for a cycle whose total latency exceeds Lambda times its total
/// distance, by Bellman-Ford longest-path relaxation on the weights
/// lat(From) - Lambda * Dist. Returns the cycle's ratio (latency over
/// distance), which is greater than Lambda, or 0 if there is no such cycle.
/// Edges are sorted by destination, and same-iteration edges go from lower to
/// higher node indices, so one sweep settles all same-iteration paths.
double betterCycleRatio(ArrayRef<CostNode> Nodes, ArrayRef<Edge> Edges,
                        double Lambda) {
  constexpr double Eps = 1e-9;
  unsigned N = Nodes.size();
  SmallVector<double, 32> Dist(N, 0.0);
  SmallVector<int, 32> Via(N, -1); // Index in Edges of the edge that set Dist.
  int Last = -1;
  for (unsigned Round = 0; Round != N + 1; ++Round) {
    Last = -1;
    for (unsigned E = 0; E != Edges.size(); ++E) {
      const Edge &Ed = Edges[E];
      double W = Nodes[Ed.From].Cost.Latency - Lambda * Ed.Dist;
      if (Dist[Ed.From] + W > Dist[Ed.To] + Eps) {
        Dist[Ed.To] = Dist[Ed.From] + W;
        Via[Ed.To] = E;
        Last = Ed.To;
      }
    }
    if (Last < 0)
      return 0;
  }
  // Still relaxing after N rounds: a positive cycle exists. Walk back N steps
  // to land inside it, then trace it.
  unsigned X = Last;
  for (unsigned I = 0; I != N; ++I) {
    assert(Via[X] >= 0 && "a node on a positive cycle was relaxed");
    X = Edges[Via[X]].From;
  }
  double Lat = 0;
  unsigned D = 0;
  unsigned Cur = X;
  do {
    const Edge &Ed = Edges[Via[Cur]];
    Lat += Nodes[Ed.From].Cost.Latency;
    D += Ed.Dist;
    Cur = Ed.From;
  } while (Cur != X);
  return D ? Lat / D : 0;
}

/// The maximum over all cycles of the loop dependence graph of (sum of node
/// latencies) / (sum of carried distances). Starting from lambda 0, each step
/// finds a cycle that beats lambda and raises lambda to its ratio, so the
/// result is exact and the loop ends after finitely many cycles.
double maxCycleRatio(ArrayRef<CostNode> Nodes) {
  SmallVector<Edge, 64> Edges;
  for (unsigned V = 0; V != Nodes.size(); ++V) {
    for (unsigned P : Nodes[V].Preds)
      Edges.push_back({P, V, 0});
    for (auto [U, D] : Nodes[V].Carried)
      Edges.push_back({U, V, D});
  }
  double Lambda = 0;
  while (double R = betterCycleRatio(Nodes, Edges, Lambda)) {
    if (R <= Lambda)
      break;
    Lambda = R;
  }
  return Lambda;
}

/// TRes of a description with port sets: the maximum over the non-empty unions
/// U of the loaded classes' port sets of (the load of the classes whose set
/// lies inside U) / |U|; a class without ports is its own pool, load / units.
/// BusiestClass is the class with the largest load inside the maximizing U.
void resourceBoundOverPorts(const PortDescription &PD, ArrayRef<double> Load,
                            RegionEstimate &E) {
  SmallVector<std::pair<uint64_t, unsigned>, 16> Sets; // (port mask, class)
  for (unsigned C = 0; C != PD.numClasses(); ++C) {
    if (Load[C] <= 0)
      continue;
    ArrayRef<unsigned> Ports = PD.ports(C, E.With512);
    if (Ports.empty()) {
      double T = Load[C] / PD.units(C);
      if (T > E.TRes) {
        E.TRes = T;
        E.BusiestClass = C;
      }
      continue;
    }
    uint64_t Mask = 0;
    for (unsigned P : Ports)
      Mask |= uint64_t(1) << P;
    Sets.push_back({Mask, C});
  }
  // The distinct sets; every union is a union of a subset of them (at most 16
  // classes list ports, so at most 2^16 subsets).
  SmallVector<uint64_t, 16> Distinct;
  for (auto &[Mask, C] : Sets)
    if (!is_contained(Distinct, Mask))
      Distinct.push_back(Mask);
  for (uint32_t Subset = 1; Subset < (uint32_t(1) << Distinct.size());
       ++Subset) {
    uint64_t U = 0;
    for (unsigned I = 0; I != Distinct.size(); ++I)
      if (Subset >> I & 1)
        U |= Distinct[I];
    double Sum = 0, Max = 0;
    unsigned Busiest = 0;
    for (auto &[Mask, C] : Sets) {
      if (Mask & ~U)
        continue;
      Sum += Load[C];
      if (Load[C] > Max) {
        Max = Load[C];
        Busiest = C;
      }
    }
    double T = Sum / llvm::popcount(U);
    if (T > E.TRes) {
      E.TRes = T;
      E.BusiestClass = Busiest;
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
  RegionEstimate E;

#ifndef NDEBUG
  // Input contract, for loop and non-loop regions alike.
  for (unsigned I = 0; I != Nodes.size(); ++I) {
    for (unsigned P : Nodes[I].Preds)
      assert(P < I && "nodes must be in topological order");
    for (auto [U, Dist] : Nodes[I].Carried)
      assert(U < Nodes.size() && Dist >= 1 && "bad carried edge");
  }
#endif

  // Resource bound: the most loaded class, per unit.
  SmallVector<double, 8> Load(PD.numClasses(), 0.0);
  double Uops = 0;
  for (const CostNode &N : Nodes) {
    for (const ResourceUse &U : N.Cost.Uses)
      Load[U.Class] += U.Cycles;
    Uops += N.Cost.Uops;
  }
  LLVM_DEBUG({
    dbgs() << "savio: class loads (cycles, whole region):";
    for (unsigned C = 0; C != PD.numClasses(); ++C)
      if (Load[C] > 0)
        dbgs() << ' ' << PD.className(C) << '=' << Load[C];
    dbgs() << " uops=" << Uops << '\n';
  });
  if (!PD.hasPortSets()) {
    for (unsigned C = 0; C != PD.numClasses(); ++C) {
      double T = Load[C] / PD.units(C);
      if (T > E.TRes) {
        E.TRes = T;
        E.BusiestClass = C;
      }
    }
  } else {
    E.With512 = any_of(Nodes, [](const CostNode &N) { return N.Wide512; });
    resourceBoundOverPorts(PD, Load, E);
  }

  // Dispatch bound.
  E.TDisp = Uops / PD.dispatchWidth();

  // Recurrence bound. In a loop it is the maximum cycle ratio over every
  // cycle of the dependence graph, however many carried edges it crosses.
  if (IsLoop) {
    E.TRec = maxCycleRatio(Nodes);
  } else {
    // Longest latency path in the region.
    SmallVector<double, 16> Dist(Nodes.size(), 0.0);
    for (unsigned I = 0; I != Nodes.size(); ++I) {
      double Best = 0;
      for (unsigned P : Nodes[I].Preds)
        Best = std::max(Best, Dist[P]);
      Dist[I] = Best + Nodes[I].Cost.Latency;
      E.TRec = std::max(E.TRec, Dist[I]);
    }
  }
  return E;
}
