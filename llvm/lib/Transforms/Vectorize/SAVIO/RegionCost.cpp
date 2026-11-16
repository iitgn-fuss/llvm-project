//===- RegionCost.cpp - SAVIO region cost model ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/RegionCost.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <cassert>

using namespace llvm;
using namespace llvm::savio;

double RegionEstimate::t() const { return std::max({TRes, TRec, TDisp}); }

Bound RegionEstimate::limiting() const {
  double T = t();
  if (TRec == T)
    return Bound::Recurrence;
  if (TRes == T)
    return Bound::Resource;
  return Bound::Dispatch;
}

/// Longest latency path from node \p From to node \p To (both included) along
/// same-iteration edges, or 0 if \p To is not reachable from \p From.
static double longestPath(ArrayRef<CostNode> Nodes, unsigned From,
                          unsigned To) {
  if (To < From)
    return 0;
  SmallVector<double, 16> Dist(To - From + 1, -1.0);
  Dist[0] = Nodes[From].Cost.Latency;
  for (unsigned I = From + 1; I <= To; ++I) {
    double Best = -1.0;
    for (unsigned P : Nodes[I].Preds)
      if (P >= From && P < I && Dist[P - From] >= 0)
        Best = std::max(Best, Dist[P - From]);
    if (Best >= 0)
      Dist[I - From] = Best + Nodes[I].Cost.Latency;
  }
  return std::max(Dist[To - From], 0.0);
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
  for (unsigned C = 0; C != PD.numClasses(); ++C) {
    double T = Load[C] / PD.units(C);
    if (T > E.TRes) {
      E.TRes = T;
      E.BusiestClass = C;
    }
  }

  // Dispatch bound.
  E.TDisp = Uops / PD.dispatchWidth();

  // Recurrence bound. Nodes are in topological order, so every same-iteration
  // predecessor has a smaller index. Only cycles through a single carried edge
  // are modeled in v1; a cycle that spans more than one carried edge is not.
  if (IsLoop) {
    for (unsigned V = 0; V != Nodes.size(); ++V)
      for (auto [U, Dist] : Nodes[V].Carried) {
        E.TRec = std::max(E.TRec, longestPath(Nodes, V, U) / Dist);
      }
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
