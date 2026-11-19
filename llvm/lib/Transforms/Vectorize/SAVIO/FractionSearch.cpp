//===- FractionSearch.cpp - SAVIO fraction and unroll search --------------===
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "PartitionerUtils.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/Transforms/Vectorize/SAVIO/SearchCore.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <cfloat>
#include <optional>

using namespace llvm;
using namespace llvm::savio;

#define DEBUG_TYPE "savio"

static void recordMoved(const Region &R, Decision &D) {
  DenseMap<unsigned, unsigned> Total;
  for (const Group &G : R.Groups)
    ++Total[G.SliceClass];
  SmallVector<ClassMove, 2> M;
  for (unsigned G : D.Promoted) {
    unsigned C = R.Groups[G].SliceClass;
    auto *It = find_if(M, [&](const ClassMove &P) { return P.Class == C; });
    if (It == M.end())
      M.push_back({C, 1, Total[C]});
    else
      ++It->K;
  }
  D.MovedPerClass = M;
}

namespace {

/// Outcome of the fraction search on the region unrolled \p Copies times.
struct SearchResult {
  unsigned Copies = 1;
  RegionEstimate Before, After; // Per original iteration.
  SmallVector<bool, 8> Promoted;  // By virtual group.
  SmallVector<ClassMove, 2> Moves;
  bool any() const { return !Moves.empty(); }
};

struct SearchCoreState {
  const Region *R;
  const RegionGraph *RG;
  const PortDescription *PD;
  FallbackCostFn Fallback;
  ArrayRef<unsigned> Classes;
  const DenseMap<unsigned, SmallVector<unsigned, 4>> *Members;
  SmallVector<bool, 8> *Promoted;
  unsigned Copies;
};

} // namespace
extern "C" double savioSearchCoreEstimate(void *Raw,
                                         const unsigned *MovedPerClass,
                                         unsigned NumClasses) {
  auto *State = static_cast<SearchCoreState *>(Raw);
  for (bool &P : *State->Promoted)
    P = false;
  for (unsigned I = 0; I != NumClasses; ++I) {
    unsigned C = State->Classes[I];
    auto It = State->Members->find(C);
    if (It == State->Members->end())
      return DBL_MAX;
    ArrayRef<unsigned> M = It->second;
    unsigned K = MovedPerClass[I];
    for (unsigned J = 0; J != K; ++J)
      (*State->Promoted)[M[size_t(J) * M.size() / K]] = true;
  }
  if (std::optional<RegionEstimate> E =
          estimateAssignment(*State->R, *State->RG, *State->PD,
                             State->Fallback, *State->Promoted,
                             State->Copies))
    return E->t();
  return DBL_MAX;
}

/// Fraction search. Unlike the greedy loop it replaces, there is no "only
/// while the resource bound limits" precondition: any bound may be lowered,
/// guarded by the margin. Per slice class, estimate every k of its U instances
/// (spread evenly) with the other classes fixed; take the best if it beats
/// the current T by the margin. Repeat over classes until nothing changes.
/// With \p Copies > 1 the region is the loop unrolled that many times (not
/// done in the IR); a recurrence stays one instance through all copies.
static SearchResult searchFraction(const Region &R, const RegionGraph &RG,
                                   const PortDescription &PD,
                                   FallbackCostFn Fallback, double Margin,
                                   unsigned Copies) {
  unsigned NG = R.Groups.size();
  SearchResult Res;
  Res.Copies = Copies;
  Res.Promoted.assign(size_t(Copies) * NG, false);
  SmallVector<bool, 8> &Promoted = Res.Promoted;
  auto Estimate = [&] {
    return estimateAssignment(R, RG, PD, Fallback, Promoted, Copies);
  };
  Res.Before = Res.After = *Estimate();

  // The movable instances of each class, as virtual groups in program order of
  // the unrolled body.
  SmallVector<unsigned, 4> Classes;
  DenseMap<unsigned, SmallVector<unsigned, 4>> Members;
  DenseSet<unsigned> RecurrenceClasses;
  for (unsigned J = 0; J != Copies; ++J)
    for (unsigned G = 0; G != NG; ++G) {
      const Group &Grp = R.Groups[G];
      if (Grp.Recurrence && J != 0)
        continue;
      unsigned C = Grp.SliceClass;
      if (Grp.Recurrence)
        RecurrenceClasses.insert(C);
      if (Members[C].empty())
        Classes.push_back(C);
      Members[C].push_back(Grp.Recurrence ? G : J * NG + G);
    }
  SmallVector<unsigned, 8> Sizes;
  SmallVector<unsigned, 8> Recurrence;
  SmallVector<unsigned, 8> Current(Classes.size(), 0);
  for (unsigned C : Classes) {
    Sizes.push_back(Members[C].size());
    Recurrence.push_back(R.Groups[Members[C][0]].Recurrence ? 1 : 0);
  }
  SearchCoreState State{
      &R, &RG, &PD, Fallback, Classes, &Members, &Promoted, Copies};
  savio_search_fraction(
      Classes.size(), Sizes.data(), Recurrence.data(), Current.data(),
      Res.After.t(), Margin, &State, savioSearchCoreEstimate, 4);
  for (unsigned I = 0; I != Classes.size(); ++I) {
    unsigned C = Classes[I];
    unsigned K = Current[I];
    ArrayRef<unsigned> M = Members[C];
    for (unsigned G : M)
      Promoted[G] = false;
    for (unsigned J = 0; J != K; ++J)
      Promoted[M[size_t(J) * M.size() / K]] = true;
  }
  if (std::optional<RegionEstimate> Final = Estimate())
    Res.After = *Final;
  for (unsigned I = 0; I != Classes.size(); ++I)
    if (Current[I])
      Res.Moves.push_back(
          {Classes[I], Current[I], unsigned(Members[Classes[I]].size())});
  return Res;
}

Decision savio::decide(const Region &R, const PortDescription &PD,
                       FallbackCostFn Fallback, double Margin,
                       bool CanUnroll) {
  unsigned N = R.Groups.size();
  SmallVector<bool, 8> Promoted(N, false);
  RegionGraph RG = buildRegionGraph(*R.BB, R.L);

  auto Estimate = [&]() -> std::optional<RegionEstimate> {
    return estimateAssignment(R, RG, PD, Fallback, Promoted, 1);
  };

  Decision D;
  D.Before = *Estimate();
  D.After = D.Before;

  if (SAVIOForce) {
    for (unsigned G = 0; G != N; ++G) {
      Promoted[G] = true;
      if (!Estimate())
        Promoted[G] = false;
      else
        D.Promoted.push_back(G);
    }
    D.After = *Estimate();
    recordMoved(R, D);
    return D;
  }

  if (SAVIOForceCount >= 0) {
    // The slice class with the most instances (the first one found on ties).
    DenseMap<unsigned, unsigned> Count;
    unsigned BestClass = 0, BestCount = 0;
    for (const Group &G : R.Groups) {
      unsigned C = ++Count[G.SliceClass];
      if (C > BestCount) {
        BestCount = C;
        BestClass = G.SliceClass;
      }
    }
    // A vector divide's instances are lane groups: the count is in lanes.
    unsigned Limit = SAVIOForceCount;
    for (const Group &G : R.Groups)
      if (G.SliceClass == BestClass && G.VectorDivide) {
        if (Limit % G.GroupLanes) {
          if (G.GroupLanes == 2)
            report_fatal_error("savio-force-count: a vector divide moves lane "
                               "pairs, so the count of lanes must be even");
          report_fatal_error("-savio-force-count=" + Twine(Limit) +
                             " must be a multiple of the lane group " +
                             Twine(G.GroupLanes));
        }
        Limit /= G.GroupLanes;
        break;
      }
    unsigned Taken = 0;
    for (unsigned G = 0; G != N && Taken < Limit; ++G) {
      if (R.Groups[G].SliceClass != BestClass)
        continue;
      Promoted[G] = true;
      if (!Estimate()) {
        Promoted[G] = false;
        continue;
      }
      D.Promoted.push_back(G);
      ++Taken;
    }
    D.After = *Estimate();
    recordMoved(R, D);
    return D;
  }

  SearchResult Base = searchFraction(R, RG, PD, Fallback, Margin, 1);
  const SearchResult *Chosen = Base.any() ? &Base : nullptr;

  // Unrolling to reach a better fraction: U instances of the most numerous
  // acyclic slice class are present now (copies); try U = 2*copies, ... <= 8,
  // every candidate searched as a whole. A candidate is taken if it moves
  // something and beats the unchanged loop by the margin; among those the
  // smallest unroll within the margin of the best wins (less code growth).
  SmallVector<SearchResult, 4> Unrolled;
  if (CanUnroll) {
    DenseMap<unsigned, unsigned> Count;
    unsigned Copies = 0;
    for (const Group &G : R.Groups)
      if (!G.Recurrence)
        Copies = std::max(Copies, ++Count[G.SliceClass]);
    for (unsigned M = 2; Copies && M * Copies <= 8; ++M) {
      SearchResult S = searchFraction(R, RG, PD, Fallback, Margin, M);
      // It must beat the unchanged loop by the margin and, if some fraction
      // of the loop as it is already pays, that one too: code growth is only
      // worth a real gain over not unrolling.
      double Ref = Chosen ? Chosen->After.t() : Base.Before.t();
      if (S.any() && Base.Before.t() - S.After.t() > 0 &&
          Base.Before.t() - S.After.t() >= Margin * Base.Before.t() &&
          Ref - S.After.t() > 0 && Ref - S.After.t() >= Margin * Ref)
        Unrolled.push_back(std::move(S));
    }
  }
  if (!Unrolled.empty()) {
    double Best = Chosen ? Chosen->After.t() : Unrolled[0].After.t();
    for (const SearchResult &S : Unrolled)
      Best = std::min(Best, S.After.t());
    const SearchResult *Pick = nullptr;
    if (Chosen && Chosen->After.t() <= Best * (1 + Margin))
      Pick = Chosen;
    for (const SearchResult &S : Unrolled)
      if (!Pick && S.After.t() <= Best * (1 + Margin))
        Pick = &S;
    Chosen = Pick;
  }

  if (!Chosen)
    return D;
  D.After = Chosen->After;
  D.Unroll = Chosen->Copies;
  D.MovedPerClass = Chosen->Moves;
  if (D.Unroll == 1)
    for (unsigned G = 0; G != N; ++G)
      if (Chosen->Promoted[G])
        D.Promoted.push_back(G);
  return D;
}
