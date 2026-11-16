//===- Partitioner.h - SAVIO groups and the greedy split search -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Splits a function into regions, grows use-def groups of in-scope scalar
// integer operations in each region, and decides which groups to Singleton
// Vectorize by greedy search on the region cost model.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONER_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONER_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Vectorize/SAVIO/RegionCost.h"

namespace llvm {
class BasicBlock;
class Function;
class Instruction;
class Loop;
class LoopInfo;
class PHINode;
class Use;

extern cl::opt<double> SAVIOMargin;
extern cl::opt<bool> SAVIOForce;

namespace savio {

/// A use-def connected set of in-scope i32 operations of one region.
struct Group {
  /// In-scope i32 operations, in program order.
  SmallVector<Instruction *, 8> Ops;
  /// Operands of Ops defined outside the group.
  SmallVector<Use *, 4> Inputs;
  /// Ops with users outside the group.
  SmallVector<Instruction *, 4> LiveOuts;
  /// Loop phis kept in vector form when the group is Singleton Vectorized.
  SmallVector<PHINode *, 2> CarriedPhis;
};

struct Region {
  BasicBlock *BB;
  Loop *L; // Null outside loops.
  SmallVector<Group, 4> Groups;
};

struct Decision {
  RegionEstimate Before, After;
  SmallVector<unsigned, 4> Promoted; // Indices into Region::Groups.
};

/// Regions of \p F that have at least one group: innermost single-block loops
/// and blocks outside loops. Blocks of other loops are not regions in v1.
SmallVector<Region, 4> formRegions(Function &F, LoopInfo &LI);

/// Chooses the groups of \p R to Singleton Vectorize. \p Margin is the
/// relative improvement of T a step must reach to be taken.
Decision decide(const Region &R, const PortDescription &PD, double Margin);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONER_H
