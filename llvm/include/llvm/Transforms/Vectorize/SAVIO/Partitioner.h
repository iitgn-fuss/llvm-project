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
/// With -savio-force-count=n the first n instances of the largest slice class
/// move; for the lane pairs of a vector divide n counts lanes (2 per pair).
extern cl::opt<int> SAVIOForceCount;

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
  /// (same operations, constants and input kinds): interchangeable. A
  /// recurrence group is a single instance and always has a class of its own.
  unsigned SliceClass = 0;
  /// True for a whole recurrence (a cycle of the dependence graph, all in
  /// scope) moved as a vector-phi group; false for an acyclic slice.
  bool Recurrence = false;
  /// True for one lane pair of a `udiv`/`urem <4 x i32>` (Ops holds that one
  /// vector instruction): Pair 0 is lanes {0,1}, Pair 1 is lanes {2,3}. The
  /// two pairs of one instruction are two instances of one slice class; moving
  /// a pair sends its lanes through fdiv .2d. Has no inputs or live-outs: the
  /// operands are never transferred, only lanes of vectors that are already
  /// vectors.
  bool VectorDivide = false;
  unsigned Pair = 0;
};

struct Region {
  BasicBlock *BB;
  Loop *L; // Null outside loops.
  SmallVector<Group, 4> Groups;
};

/// k of the U instances of a slice class are moved.
struct ClassMove {
  unsigned Class, K, U;
};

struct Decision {
  RegionEstimate Before, After;
  /// Indices into Region::Groups (of the region as it is now).
  SmallVector<unsigned, 4> Promoted;
  /// Unroll factor to apply before the instances are moved; with more than 1,
  /// Promoted is empty and MovedPerClass counts instances of the unrolled
  /// loop. Before/After are per original iteration either way.
  unsigned Unroll = 1;
  /// Classes with at least one instance moved.
  SmallVector<ClassMove, 2> MovedPerClass;
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
/// With \p CanUnroll the loop of \p R may be virtually unrolled to reach a
/// better fraction (the caller has checked that it can be unrolled for real);
/// the result then has Unroll > 1 and no Promoted: unroll, re-form the
/// regions, and decide again on the unrolled loop.
Decision decide(const Region &R, const PortDescription &PD,
                FallbackCostFn Fallback, double Margin, bool CanUnroll = false);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONER_H
