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

static cl::opt<unsigned> SAVIOMaxVirtualUnrollInstances(
    "savio-max-virtual-unroll-instances", cl::init(16), cl::Hidden,
    cl::desc("Maximum virtual unroll instances explored in fraction search"));

/// Populates \p D.MovedPerClass with the summary of how many instances of each
/// slice class were promoted from the scalar unit to the vector unit.
static void recordMoved(const Region &R, Decision &D) {
  DenseMap<unsigned, unsigned> ClassTotalCounts;
  for (const Group &Grp : R.Groups)
    ++ClassTotalCounts[Grp.SliceClass];

  SmallVector<ClassMove, 2> ClassMoves;
  for (unsigned GroupIdx : D.Promoted) {
    unsigned SliceClass = R.Groups[GroupIdx].SliceClass;
    auto *It = find_if(ClassMoves, [&](const ClassMove &Move) {
      return Move.Class == SliceClass;
    });
    if (It == ClassMoves.end())
      ClassMoves.push_back({SliceClass, 1, ClassTotalCounts[SliceClass]});
    else
      ++It->K;
  }
  D.MovedPerClass = std::move(ClassMoves);
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

/// State passed to the C branch-and-bound fraction search callback.
struct SearchCoreState {
  /// The input region being analyzed.
  const Region *R;
  /// Intra-iteration and carried dependency graph of the region.
  const RegionGraph *RG;
  /// Target execution-port and cost description.
  const PortDescription *PD;
  /// Fallback cost callback for operations without port descriptions.
  FallbackCostFn Fallback;
  /// List of slice class IDs under evaluation.
  ArrayRef<unsigned> Classes;
  /// Map from each slice class ID to its list of candidate group indices
  /// in the (virtually unrolled) region.
  const DenseMap<unsigned, SmallVector<unsigned, 4>> *Members;
  /// Output bitmask of promoted virtual groups.
  SmallVector<bool, 8> *Promoted;
  /// Number of virtual loop copies (virtual unroll factor).
  unsigned Copies;
};

} // namespace

/// Evaluation callback passed to pure-C fraction search (`savio_search_fraction`).
/// Maps the candidate slice movement counts `MovedPerClass` onto the virtual group
/// promotion bitmask, distributes moved instances evenly across the unrolled
/// iterations, and invokes `estimateAssignment`.
extern "C" double savioSearchCoreEstimate(void *RawState,
                                         const unsigned *MovedPerClass,
                                         unsigned NumClasses) {
  auto *State = static_cast<SearchCoreState *>(RawState);

  // Clear all promotions.
  for (bool &IsPromoted : *State->Promoted)
    IsPromoted = false;

  // Distribute the requested number of promotions per slice class evenly.
  for (unsigned I = 0; I != NumClasses; ++I) {
    unsigned SliceClass = State->Classes[I];
    auto It = State->Members->find(SliceClass);
    if (It == State->Members->end())
      return DBL_MAX;

    ArrayRef<unsigned> ClassGroupIndices = It->second;
    unsigned NumPromotedInClass = MovedPerClass[I];
    for (unsigned PromotedIdx = 0; PromotedIdx != NumPromotedInClass; ++PromotedIdx) {
      // Pick elements spaced evenly across the total available instances in this class.
      size_t SelectedIdx =
          size_t(PromotedIdx) * ClassGroupIndices.size() / NumPromotedInClass;
      (*State->Promoted)[ClassGroupIndices[SelectedIdx]] = true;
    }
  }

  // Evaluate the resulting candidate assignment across all unrolled copies.
  if (std::optional<RegionEstimate> Est =
          estimateAssignment(*State->R, *State->RG, *State->PD,
                             State->Fallback, *State->Promoted,
                             State->Copies))
    return Est->t();

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
  unsigned NumGroups = R.Groups.size();
  SearchResult Result;
  Result.Copies = Copies;
  Result.Promoted.assign(size_t(Copies) * NumGroups, false);
  SmallVector<bool, 8> &Promoted = Result.Promoted;
  auto Estimate = [&] {
    return estimateAssignment(R, RG, PD, Fallback, Promoted, Copies);
  };
  Result.Before = Result.After = *Estimate();

  // Phase 1: Build virtual candidate groups in program order of the unrolled body.
  // Note: Acyclic groups replicate for each of the `Copies` unrolled iterations
  // at virtual index (CopyIdx * NumGroups + GroupIdx).
  // Recurrence groups appear only once (at index GroupIdx) across all copies.
  SmallVector<unsigned, 4> Classes;
  DenseMap<unsigned, SmallVector<unsigned, 4>> Members;
  DenseSet<unsigned> RecurrenceClasses;
  for (unsigned CopyIdx = 0; CopyIdx != Copies; ++CopyIdx) {
    for (unsigned GroupIdx = 0; GroupIdx != NumGroups; ++GroupIdx) {
      const Group &Grp = R.Groups[GroupIdx];
      if (Grp.Recurrence && CopyIdx != 0)
        continue;
      unsigned SliceClass = Grp.SliceClass;
      if (Grp.Recurrence)
        RecurrenceClasses.insert(SliceClass);
      if (Members[SliceClass].empty())
        Classes.push_back(SliceClass);
      Members[SliceClass].push_back(Grp.Recurrence ? GroupIdx
                                                  : CopyIdx * NumGroups + GroupIdx);
    }
  }

  // Phase 2: Prepare inputs for the pure-C fraction search solver.
  SmallVector<unsigned, 8> ClassSizes;
  SmallVector<unsigned, 8> ClassIsRecurrence;
  SmallVector<unsigned, 8> ClassPromotedCounts(Classes.size(), 0);
  for (unsigned SliceClass : Classes) {
    ClassSizes.push_back(Members[SliceClass].size());
    ClassIsRecurrence.push_back(
        R.Groups[Members[SliceClass][0]].Recurrence ? 1 : 0);
  }

  SearchCoreState State{
      &R, &RG, &PD, Fallback, Classes, &Members, &Promoted, Copies};

  // Phase 3: Solve for the optimal count of promoted instances per class.
  constexpr unsigned RoundLimit = 4;
  savio_search_fraction(
      Classes.size(), ClassSizes.data(), ClassIsRecurrence.data(),
      ClassPromotedCounts.data(), Result.After.t(), Margin, &State,
      savioSearchCoreEstimate, RoundLimit);

  // Phase 4: Decode optimal per-class promotions back into the Promoted bitmask.
  for (unsigned I = 0; I != Classes.size(); ++I) {
    unsigned SliceClass = Classes[I];
    unsigned NumPromotedInClass = ClassPromotedCounts[I];
    ArrayRef<unsigned> ClassGroupIndices = Members[SliceClass];
    for (unsigned GroupIdx : ClassGroupIndices)
      Promoted[GroupIdx] = false;
    for (unsigned PromotedIdx = 0; PromotedIdx != NumPromotedInClass; ++PromotedIdx) {
      size_t SelectedIdx =
          size_t(PromotedIdx) * ClassGroupIndices.size() / NumPromotedInClass;
      Promoted[ClassGroupIndices[SelectedIdx]] = true;
    }
  }

  // Phase 5: Re-evaluate final assignment and record moves.
  if (std::optional<RegionEstimate> Final = Estimate())
    Result.After = *Final;
  for (unsigned I = 0; I != Classes.size(); ++I) {
    if (ClassPromotedCounts[I])
      Result.Moves.push_back({Classes[I], ClassPromotedCounts[I],
                              unsigned(Members[Classes[I]].size())});
  }

  return Result;
}

Decision savio::decide(const Region &R, const PortDescription &PD,
                       FallbackCostFn Fallback, double Margin,
                       bool CanUnroll) {
  unsigned NumGroups = R.Groups.size();
  SmallVector<bool, 8> Promoted(NumGroups, false);
  RegionGraph RG = buildRegionGraph(*R.BB, R.L);

  auto Estimate = [&]() -> std::optional<RegionEstimate> {
    return estimateAssignment(R, RG, PD, Fallback, Promoted, 1);
  };

  Decision Dec;
  Dec.Before = *Estimate();
  Dec.After = Dec.Before;

  // Manual override: force promotion of all candidate groups where valid.
  if (SAVIOForce) {
    for (unsigned GroupIdx = 0; GroupIdx != NumGroups; ++GroupIdx) {
      Promoted[GroupIdx] = true;
      if (!Estimate())
        Promoted[GroupIdx] = false;
      else
        Dec.Promoted.push_back(GroupIdx);
    }
    Dec.After = *Estimate();
    recordMoved(R, Dec);
    return Dec;
  }

  // Manual override: force promotion of a fixed number of instances of the
  // most prevalent slice class.
  if (SAVIOForceCount >= 0) {
    DenseMap<unsigned, unsigned> ClassInstanceCounts;
    unsigned BestClass = 0, BestCount = 0;
    for (const Group &Grp : R.Groups) {
      unsigned Count = ++ClassInstanceCounts[Grp.SliceClass];
      if (Count > BestCount) {
        BestCount = Count;
        BestClass = Grp.SliceClass;
      }
    }

    // A vector divide's instances are lane groups: the count is in lanes.
    unsigned Limit = SAVIOForceCount;
    for (const Group &Grp : R.Groups) {
      if (Grp.SliceClass == BestClass && Grp.VectorDivide) {
        if (Limit % Grp.GroupLanes) {
          if (Grp.GroupLanes == 2)
            report_fatal_error("savio-force-count: a vector divide moves lane "
                               "pairs, so the count of lanes must be even");
          report_fatal_error("-savio-force-count=" + Twine(Limit) +
                             " must be a multiple of the lane group " +
                             Twine(Grp.GroupLanes));
        }
        Limit /= Grp.GroupLanes;
        break;
      }
    }

    unsigned Taken = 0;
    for (unsigned GroupIdx = 0; GroupIdx != NumGroups && Taken < Limit; ++GroupIdx) {
      if (R.Groups[GroupIdx].SliceClass != BestClass)
        continue;
      Promoted[GroupIdx] = true;
      if (!Estimate()) {
        Promoted[GroupIdx] = false;
        continue;
      }
      Dec.Promoted.push_back(GroupIdx);
      ++Taken;
    }
    Dec.After = *Estimate();
    recordMoved(R, Dec);
    return Dec;
  }

  // Phase 1: Search fractional assignments on the single (unrolled=1) loop body.
  SearchResult Base = searchFraction(R, RG, PD, Fallback, Margin, 1);
  const SearchResult *Chosen = Base.any() ? &Base : nullptr;

  // Phase 2: Explore virtual loop unrolling to reach fractional ratios not
  // representable in a single iteration.
  // U instances of the most numerous acyclic slice class are present now (copies);
  // try U = 2*copies, ... <= 8, every candidate searched as a whole.
  // A candidate is accepted only if it moves something and beats the unchanged
  // loop by Margin, and also beats the non-unrolled result by Margin.
  SmallVector<SearchResult, 4> Unrolled;
  if (CanUnroll) {
    DenseMap<unsigned, unsigned> ClassCounts;
    unsigned MaxInstancesPerClass = 0;
    for (const Group &Grp : R.Groups) {
      if (!Grp.Recurrence)
        MaxInstancesPerClass =
            std::max(MaxInstancesPerClass, ++ClassCounts[Grp.SliceClass]);
    }

    for (unsigned UnrollMult = 2;
         MaxInstancesPerClass &&
         UnrollMult * MaxInstancesPerClass <= SAVIOMaxVirtualUnrollInstances;
         ++UnrollMult) {
      SearchResult Candidate =
          searchFraction(R, RG, PD, Fallback, Margin, UnrollMult);

      // It must beat the unchanged loop by Margin and, if some fraction of the
      // loop as it is already pays, beat that one too. Code growth is only
      // justified by a real gain over not unrolling.
      double Ref = Chosen ? Chosen->After.t() : Base.Before.t();
      if (Candidate.any() && Base.Before.t() - Candidate.After.t() > 0 &&
          Base.Before.t() - Candidate.After.t() >= Margin * Base.Before.t() &&
          Ref - Candidate.After.t() > 0 && Ref - Candidate.After.t() >= Margin * Ref)
        Unrolled.push_back(std::move(Candidate));
    }
  }

  // Phase 3: Among unrolled candidates meeting the margin threshold, pick the
  // smallest unroll factor within Margin of the best candidate to limit code growth.
  if (!Unrolled.empty()) {
    double BestTime = Chosen ? Chosen->After.t() : Unrolled[0].After.t();
    for (const SearchResult &Cand : Unrolled)
      BestTime = std::min(BestTime, Cand.After.t());

    const SearchResult *Pick = nullptr;
    if (Chosen && Chosen->After.t() <= BestTime * (1 + Margin))
      Pick = Chosen;
    for (const SearchResult &Cand : Unrolled) {
      if (!Pick && Cand.After.t() <= BestTime * (1 + Margin))
        Pick = &Cand;
    }
    Chosen = Pick;
  }

  if (!Chosen)
    return Dec;

  Dec.After = Chosen->After;
  Dec.Unroll = Chosen->Copies;
  Dec.MovedPerClass = Chosen->Moves;
  if (Dec.Unroll == 1) {
    for (unsigned GroupIdx = 0; GroupIdx != NumGroups; ++GroupIdx) {
      if (Chosen->Promoted[GroupIdx])
        Dec.Promoted.push_back(GroupIdx);
    }
  }
  return Dec;
}
