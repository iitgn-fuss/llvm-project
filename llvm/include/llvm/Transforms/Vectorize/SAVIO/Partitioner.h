//===- Partitioner.h - SAVIO groups and the fraction search -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Splits a function into regions, identifies use-def connected groups of
/// candidate scalar integer operations (Singleton Vectorize candidates) and
/// vector integer operations (Scalarize candidates), and decides which groups
/// to transform based on execution port pressure and loop recurrence bounds.
///
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
class LoadInst;
class LoopInfo;
class StoreInst;
class PHINode;
class Use;
class User;
class Value;

extern cl::opt<double> SAVIOMargin;
extern cl::opt<bool> SAVIOForce;
/// With -savio-force-count=n the first n instances of the largest slice class
/// move (whatever its direction); for the lane pairs of a vector divide n counts
/// lanes (2 per pair), so an odd n is a fatal error there.
extern cl::opt<int> SAVIOForceCount;

namespace savio {

/// Direction in which an instruction group is transformed.
enum class Direction {
  /// Shift scalar operations to vector lane 0 (Singleton Vectorize).
  ToVector,
  /// Split vector operations into scalar operations across lanes (Scalarize).
  ToScalar
};

/// A connected set of candidate operations in one region that are transformed
/// together. A group contains either scalar operations (Dir == ToVector) or
/// vector operations (Dir == ToScalar), never a mixture of both.
struct Group {
  /// Transformation direction for this group.
  Direction Dir = Direction::ToVector;

  /// In-scope candidate operations belonging to this group, in program order.
  SmallVector<Instruction *, 8> Ops;

  /// ToScalar only: vector loads in the region whose users are all inside Ops.
  /// When scalarized, these become scalar loads and do not require lane extracts.
  SmallVector<LoadInst *, 2> OwnedLoads;

  /// ToScalar only: vector stores in the region storing a live-out of Ops where
  /// every user outside Ops is such a store. When scalarized, these become scalar
  /// stores directly from scalar registers without lane inserts.
  SmallVector<StoreInst *, 2> OwnedStores;

  /// Operands of Ops that are defined outside this group.
  SmallVector<Use *, 4> Inputs;

  /// Operations in Ops that have users outside this group.
  SmallVector<Instruction *, 4> LiveOuts;

  /// Loop PHI nodes kept in vector form when a recurrence group is Singleton
  /// Vectorized.
  SmallVector<PHINode *, 2> CarriedPhis;

  /// Groups with equal SliceClass represent interchangeable instances of the
  /// same code slice (identical opcodes, constant operands, and input patterns).
  /// A recurrence group is always a unique instance with its own class.
  unsigned SliceClass = 0;

  /// True if this group forms a self-contained recurrence cycle in the loop
  /// dependence graph.
  bool Recurrence = false;

  /// True for a lane group of a vector integer divide (`udiv`/`urem <N x i32>`).
  /// Ops contains that single vector divide instruction. LaneGroup identifies
  /// which lane chunk is processed (lanes [LaneGroup * GroupLanes,
  /// (LaneGroup + 1) * GroupLanes - 1]).
  bool VectorDivide = false;
  unsigned LaneGroup = 0;
  unsigned GroupLanes = 2;
};

/// An optimization region: either the body of a single-block innermost loop,
/// or a basic block outside any loop.
struct Region {
  /// The basic block corresponding to this region.
  BasicBlock *BB = nullptr;

  /// The innermost loop containing BB, or nullptr if BB is outside loops.
  Loop *L = nullptr;

  /// Candidate transformation groups formed within this region.
  SmallVector<Group, 4> Groups;

  /// Non-lowerable memory throughput floor in cycles per iteration from
  /// data-dependent indirect memory accesses (e.g. gathers/scatters); 0 if none.
  double MemFloor = 0;
};

/// Summary of how many instances of a slice class are chosen to move.
struct ClassMove {
  unsigned Class = 0; ///< The slice class identifier.
  unsigned K = 0;     ///< Number of instances moved.
  unsigned U = 0;     ///< Total number of instances of this class in the region.
};

/// The partition decision produced by the cost model and fraction search.
struct Decision {
  /// Estimated execution time and limiting bound before and after transformation.
  RegionEstimate Before, After;

  /// Indices into Region::Groups indicating which groups are selected to move.
  SmallVector<unsigned, 4> Promoted;

  /// Unroll factor required before moving instances. If Unroll > 1, the pass
  /// unrolls the loop first before re-evaluating and applying the move.
  unsigned Unroll = 1;

  /// Record of moved instances per slice class.
  SmallVector<ClassMove, 2> MovedPerClass;
};

/// Categorizes how an external input reaches a Singleton Vectorized group.
enum class InputKind {
  /// Input is free (constant, hoisted loop-invariant, or owned load).
  Free,
  /// Input resides in a GPR and requires an insert into vector lane 0.
  GprToVec,
  /// Input resides in another vector register lane and requires a lane shuffle.
  LaneToLane0
};

/// Classifies the operand \p V of an operation in \p G which is defined outside
/// \p G. \p L is the enclosing loop, or nullptr if outside loops.
InputKind classifyInput(const Loop *L, const Group &G, const Value *V);

/// Returns true if user \p U is an operation or carried PHI inside group \p G.
bool isInGroup(const Group &G, const User *U);

/// Returns true if the live-out instruction \p I of group \p G requires a
/// vector-to-GPR transfer because some external user is not a simple store.
bool liveOutNeedsGpr(const Group &G, const Instruction &I);

/// For ToScalar groups: returns true if live-out \p I needs its scalar lanes
/// re-inserted into a vector because some user is not an owned scalar store.
bool liveOutNeedsInsert(const Group &G, const Instruction &I);

/// Identifies candidate regions in function \p F (single-block innermost loops
/// and straight-line blocks outside loops) and forms their candidate groups.
/// \p LaneGroup specifies the number of lanes per FP-divider group.
SmallVector<Region, 4> formRegions(Function &F, LoopInfo &LI,
                                   unsigned LaneGroup = 2,
                                   const PortDescription *PD = nullptr);

/// Instruction dependence graph for a region's basic block.
///
/// Non-PHI instructions form nodes in program order, guaranteeing a topological
/// order for same-iteration dataflow edges. The loop-control branch terminator
/// is included only for loop regions. Loop-carried dependencies through header
/// PHIs are stored as carried edges with an iteration distance.
struct RegionGraph {
  /// Semantic type aliases for carried dependencies:
  /// (ProducerNodeIndex, IterationDistance >= 1).
  using NodeIndex = unsigned;
  using IterationDistance = unsigned;
  using CarriedDep = std::pair<NodeIndex, IterationDistance>;

  /// Instructions corresponding to graph nodes, in program order.
  SmallVector<const Instruction *, 32> Insts;

  /// Map from instruction pointer to its node index in Insts.
  DenseMap<const Instruction *, unsigned> Index;

  /// Same-iteration predecessors for each node (intra-iteration def-use edges).
  SmallVector<SmallVector<unsigned, 2>, 32> Preds;

  /// Loop-carried dependencies incoming to each node:
  /// pairs of (ProducerNodeIndex, IterationDistance >= 1).
  SmallVector<SmallVector<CarriedDep, 1>, 32> Carried;

  /// For each header PHI whose latch value originates inside this block:
  /// maps the PHI to (LatchProducerNodeIndex, IterationDistance).
  DenseMap<const PHINode *, CarriedDep> PhiLatch;
};

/// Constructs the instruction dependence graph for basic block \p BB.
RegionGraph buildRegionGraph(BasicBlock &BB, Loop *L);

/// Fallback cost function for instructions not explicitly modelled in port-info.
using FallbackCostFn = function_ref<OpCost(const Instruction &)>;

/// Evaluates candidate groups in \p R and chooses the partition that minimizes
/// estimated execution time.
Decision decide(const Region &R, const PortDescription &PD,
                FallbackCostFn Fallback, double Margin, bool CanUnroll = false);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_PARTITIONER_H
