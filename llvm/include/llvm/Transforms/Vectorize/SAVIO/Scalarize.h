//===- Scalarize.h - SAVIO Scalarize rewrite --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Rewrites a group of `<4 x i32>` / `<2 x i64>` vector operations into one
// scalar operation per lane.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_SCALARIZE_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_SCALARIZE_H

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"

namespace llvm {
class Loop;

namespace savio {

/// Scalarizes \p G, a ToScalar group of a region of the loop \p L (null
/// outside loops). The values enter and leave the group exactly as the cost
/// model charged: the owned loads and stores become per-lane scalar accesses
/// (alignment derived per lane, aliasing metadata copied), constants and
/// loop-invariant values (extracted once in the preheader, if there is one)
/// are free, other vector inputs are extracted lane by lane, and live-outs
/// that are not owned stores are rebuilt with insertelement chains. The vector
/// operations, owned loads and owned stores are erased.
void rewriteScalarize(const Group &G, Loop *L);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_SCALARIZE_H
