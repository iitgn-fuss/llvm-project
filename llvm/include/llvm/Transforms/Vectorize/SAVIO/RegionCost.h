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
  /// Loop-carried dependences as (predecessor index, distance >= 1).
  SmallVector<std::pair<unsigned, unsigned>, 1> Carried;
};

enum class Bound { Resource, Recurrence, Dispatch };

struct RegionEstimate {
  double TRes = 0;
  double TRec = 0;
  double TDisp = 0;
  unsigned BusiestClass = 0;

  /// Estimated cycles: the largest of the three bounds.
  double t() const;
  /// The bound that determines t(); ties resolve Recurrence > Resource >
  /// Dispatch.
  Bound limiting() const;
};

/// Estimates \p Nodes on \p PD. Only the given nodes are charged; the caller
/// is responsible for creating transfer nodes where a value crosses domains
/// (and none for constants, arguments or loop-invariant operands).
RegionEstimate estimateRegion(ArrayRef<CostNode> Nodes,
                              const PortDescription &PD, bool IsLoop);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_REGIONCOST_H
