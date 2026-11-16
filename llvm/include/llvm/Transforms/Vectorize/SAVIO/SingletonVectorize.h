//===- SingletonVectorize.h - SAVIO Singleton Vectorize rewrite -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Rewrites a group of scalar i32 operations into llvm.savio.sv.* intrinsics on
// <4 x i32> values that hold the scalar in lane 0.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_SINGLETONVECTORIZE_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_SINGLETONVECTORIZE_H

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"

namespace llvm {
class Loop;

namespace savio {

/// Singleton Vectorizes \p G, a group of a region of the loop \p L (null
/// outside loops). The values enter and leave the group exactly as classified
/// by classifyInput and liveOutNeedsGpr, which is what the cost model charged.
/// The scalar operations and joined carried phis of the group are erased.
void rewriteSingletonVectorize(const Group &G, Loop *L);

/// Sends the lane pairs of \p I, a `udiv` or `urem <4 x i32>`, selected by
/// \p PairMask through the floating-point divider: 3 (both pairs) is one
/// llvm.savio.fpdiv4.*; 1 (lanes {0,1}) is llvm.savio.fpdiv2.* and the other
/// lanes stay `udiv`/`urem` on scalars. Lanes {2,3} alone (2) are not
/// supported: the decision never chooses them. \p I is replaced and erased.
void rewriteVectorDivide(Instruction *I, unsigned PairMask);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_SINGLETONVECTORIZE_H
