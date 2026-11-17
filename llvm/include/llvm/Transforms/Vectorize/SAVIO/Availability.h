//===- Availability.h - SAVIO per-transformation key requirements -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Which port-info keys a transformation of an operation needs. A candidate of
// a transformation is formed only if every one of them is costed, so a
// port-info directory that lacks a key makes that transformation unavailable
// for that operation and type instead of failing later at costing.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_AVAILABILITY_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_AVAILABILITY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace llvm {
class Type;

namespace savio {
class PortDescription;

enum class Transform { SingletonVectorize, FpDivideLanes, Scalarize };

/// Name of \p X in debug output.
StringRef transformName(Transform X);

/// The keys \p X needs for operation \p Op (an IR opcode name, with ".imm"
/// appended for a shift by a constant amount) on type \p Ty: the scalar type
/// for Singleton Vectorize, the fixed vector type for the other two. \p L is
/// the lane count of the fp-divide sequence (FpDivideLanes only).
///   SingletonVectorize: "sv.<op> v4i32", "gpr_to_vec", "vec_to_gpr".
///   FpDivideLanes:      "<op> v<N>i32", "fpdiv<L>.<op> v<N>i32".
///   Scalarize:          "<op> v<N>i<W>", "<op> i<W>", "load"/"store" of both
///                       types, "lane_extract", "lane_insert".
/// A key naming a transfer ("gpr_to_vec", "vec_to_gpr", "lane_extract",
/// "lane_insert") is looked up in transfers.json, any other in probes.json.
SmallVector<std::string, 8> requiredKeys(Transform X, StringRef Op, Type *Ty,
                                         unsigned L = 0);

/// The first of \p Keys that \p PD cannot cost (a probe key without irOpCost,
/// a transfer without transferCost), or nullopt if all can. "lane_extract" and
/// "lane_insert" are also satisfied by the per-width entry for \p VTy (a
/// typeKey such as "v8i32"; empty: untyped keys only).
std::optional<std::string> firstMissing(const PortDescription &PD,
                                        ArrayRef<std::string> Keys,
                                        StringRef VTy = "");

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_AVAILABILITY_H
