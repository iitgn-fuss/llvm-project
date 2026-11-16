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

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_SINGLETONVECTORIZE_H
