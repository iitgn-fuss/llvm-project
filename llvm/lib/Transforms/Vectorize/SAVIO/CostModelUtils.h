//===- CostModelUtils.h - Shared assignment-cost helpers --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_COSTMODELUTILS_H
#define LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_COSTMODELUTILS_H

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include <optional>
#include <string>

namespace llvm {
namespace savio {

unsigned laneCount(const Group &G);
std::string laneTypeName(const Group &G, bool Vector);
const Instruction *fusibleShift(const Instruction &I, const Group &G,
                                bool Vector);
std::optional<OpCost> scalarizeOpCost(const PortDescription &PD,
                                      const Instruction &I, const Group &G,
                                      bool Vector);
OpCost lanes(OpCost C, unsigned Count);
bool scalarizeInputIsFree(const Loop *L, const Group &G, const Value *V);

} // namespace savio
} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_VECTORIZE_SAVIO_COSTMODELUTILS_H
