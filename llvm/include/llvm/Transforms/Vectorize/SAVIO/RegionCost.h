//===- RegionCost.h - SAVIO region cost model -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Estimates the cycles per iteration (or per execution, outside a loop) of a
// region as the maximum of a resource bound, a recurrence bound and a dispatch
// bound.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_REGIONCOST_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_REGIONCOST_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include <utility>

namespace llvm {
namespace savio {

/// One operation (or transfer) of a region.
struct CostNode {
  OpCost Cost;
  /// Same-iteration dependences, as indices of earlier nodes (the node array
  /// must be in topological order).
  SmallVector<unsigned, 2> Preds;
  /// Loop-carried dependences as (predecessor index, distance >= 1). The edge
  /// is stored on the consumer: (u, d) on this node means the value of
  /// Nodes[u] from d iterations earlier is an operand of this node.
  SmallVector<std::pair<unsigned, unsigned>, 1> Carried;
  /// Set for a node that is an instance of a sequence entry with a concurrency
  /// table (see applyConcurrency); null otherwise.
  const ConcurrencyTable *Conc = nullptr;
  /// The node is an operation on a 512-bit vector type; any such node makes
  /// the whole region use the classes' ports_with_512.
  bool Wide512 = false;
};

enum class Bound { Resource, Recurrence, Dispatch };

struct RegionEstimate {
  double TRes = 0;
  double TRec = 0;
  double TDisp = 0;
  /// The class with the largest load inside the maximizing port union (or
  /// the most loaded class when the description has no port sets).
  unsigned BusiestClass = 0;
  /// Some node is an operation on a 512-bit vector type (only computed when
  /// the description has port sets).
  bool With512 = false;

  /// Estimated cycles: the largest of the three bounds.
  double t() const;
  /// The bound that determines t(); ties resolve Recurrence > Resource >
  /// Dispatch.
  Bound limiting() const;
};

/// Number of sequence instances effectively in flight together: \p NK nodes of
/// one sequence key in the region, \p UopsPerIter the micro-ops of all the
/// region's nodes, \p W the instruction window in micro-ops (0 = unknown). In
/// a loop, min(8, NK * max(1, W / UopsPerIter)): iterations overlap while the
/// window holds more than one; outside a loop, NK.
double effectiveConcurrency(unsigned NK, double UopsPerIter, unsigned W,
                            bool IsLoop);

/// Second pass over the nodes of an assignment: every node with a concurrency
/// table gets the per-instance cycles of its table at m_eff on its first use
/// (the saturating class). Nodes without a table are untouched.
void applyConcurrency(SmallVectorImpl<CostNode> &Nodes,
                      const PortDescription &PD, bool IsLoop);

/// Estimates \p Nodes on \p PD. Only the given nodes are charged; the caller
/// is responsible for creating transfer nodes where a value crosses domains
/// (and none for constants, arguments or loop-invariant operands).
RegionEstimate estimateRegion(ArrayRef<CostNode> Nodes,
                              const PortDescription &PD, bool IsLoop);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_REGIONCOST_H
