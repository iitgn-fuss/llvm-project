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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
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
class User;
class Value;

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
  /// Groups of one function with equal class are instances of the same slice
  /// (same operations, constants and input kinds): interchangeable.
  unsigned SliceClass = 0;
  /// True for a whole recurrence (a cycle of the dependence graph, all in
  /// scope) moved as a vector-phi group; false for an acyclic slice.
  bool Recurrence = false;
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

/// How an operand of a Singleton Vectorized group reaches it. The cost model
/// charges exactly this boundary and the rewrite materializes exactly it.
enum class InputKind { Free, GprToVec, LaneToLane0 };

/// Classifies the operand \p V of an operation of \p G, which is not itself
/// in \p G. \p L is the loop of the region, or null.
InputKind classifyInput(const Loop *L, const Group &G, const Value *V);

/// True if the user \p U of a live-out of \p G is in \p G (an operation or a
/// carried phi).
bool isInGroup(const Group &G, const User *U);

/// True if the live-out \p I of \p G needs a vector-to-GPR transfer: some user
/// outside \p G is not a simple store of it. Otherwise every such user stores
/// lane 0 straight from the vector.
bool liveOutNeedsGpr(const Group &G, const Instruction &I);

/// Regions of \p F that have at least one group: innermost single-block loops
/// and blocks outside loops. Blocks of other loops are not regions in v1.
SmallVector<Region, 4> formRegions(Function &F, LoopInfo &LI);

/// Dependences between the instructions of a region's block, independent of
/// costing. Every non-phi instruction is a node, in program order, so node
/// indices are topologically ordered for same-iteration edges; the terminator
/// (the loop-control branch) is a node only in a loop region. Header phis are
/// not nodes: a phi becomes carried edges from its latch value to its users.
struct RegionGraph {
  SmallVector<const Instruction *, 32> Insts;
  DenseMap<const Instruction *, unsigned> Index;
  /// Same-iteration predecessors of each node (operands defined in the block).
  SmallVector<SmallVector<unsigned, 2>, 32> Preds;
  /// Loop-carried edges stored on the consumer, as in CostNode::Carried:
  /// (producer node, distance >= 1). Empty outside loops.
  SmallVector<SmallVector<std::pair<unsigned, unsigned>, 1>, 32> Carried;
  /// For each header phi whose latch value is an instruction of the block:
  /// that value's node and the distance (more than 1 through a phi of a phi).
  DenseMap<const PHINode *, std::pair<unsigned, unsigned>> PhiLatch;
};

/// Builds the dependence graph of \p BB, the block of the loop \p L (or of no
/// loop if null).
RegionGraph buildRegionGraph(BasicBlock &BB, Loop *L);

/// Cost of an instruction without a port-info entry: latency and micro-ops, no
/// resource use.
using FallbackCostFn = function_ref<OpCost(const Instruction &)>;

/// Chooses the groups of \p R to Singleton Vectorize. \p Fallback costs the
/// instructions that SAVIO does not model (loads, stores, calls, other types);
/// they carry latency into the recurrence bound and micro-ops into dispatch.
/// \p Margin is the relative improvement of T a step must reach to be taken.
Decision decide(const Region &R, const PortDescription &PD,
                FallbackCostFn Fallback, double Margin);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONER_H
