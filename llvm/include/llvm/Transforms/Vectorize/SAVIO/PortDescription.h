//===- PortDescription.h - SAVIO execution-port description -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Loads the per-CPU port-info files (resources.json, ops.json, transfers.json,
// probes.json) that describe execution-resource classes and the resource use
// and latency of machine opcodes, and maps IR-level operation forms onto them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_PORTDESCRIPTION_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_PORTDESCRIPTION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <optional>
#include <string>

namespace llvm {
class TargetTransformInfo;
class Triple;
class Type;

namespace savio {

/// Cycles of occupancy on one resource class.
struct ResourceUse {
  unsigned Class;
  double Cycles;
};

/// Resource use and latency of an operation (one or more machine opcodes).
struct OpCost {
  SmallVector<ResourceUse, 2> Uses;
  unsigned Latency;
  unsigned Uops;
};

/// Cycles per instance of a sequence in its saturating class as a function of
/// the number of independent instances in flight (ops.json
/// "throughput_by_concurrency", m strictly ascending).
struct ConcurrencyTable {
  SmallVector<std::pair<unsigned, double>, 6> Points;
  /// Piecewise-linear in \p M, clamped to the range of the table.
  double at(double M) const;
};

/// Semantic data movements between the scalar and vector domains.
enum class TransferKind {
  GprToVec,
  VecToGpr,
  LoadToVec,
  StoreFromVec,
  LaneToLane0,
  /// Vector lane k to a general register (umov / fmov); optional, Scalarize.
  LaneExtract,
  /// General register to vector lane k (ins); optional, Scalarize.
  LaneInsert
};

constexpr unsigned NumTransferKinds = 7;

class PortDescription {
public:
  /// Loads <Root>/<Arch>/<CPU>/{resources,ops,transfers,probes}.json. Returns
  /// no_such_file_or_directory if the CPU directory is missing, and a
  /// StringError naming the file and key if the content is malformed.
  static Expected<std::unique_ptr<PortDescription>>
  load(StringRef Root, StringRef Arch, StringRef CPU);

  unsigned numClasses() const { return ClassNames.size(); }
  StringRef className(unsigned C) const { return ClassNames[C]; }
  unsigned units(unsigned C) const { return ClassUnits[C]; }
  /// The issue ports class \p C draws on (resources.json "ports"), or, when
  /// \p With512 and the class has "ports_with_512", the ports it uses while a
  /// 512-bit vector op is in flight (not necessarily a subset). Empty when the
  /// class has no "ports": it is then a separate pool of units(C) units.
  ArrayRef<unsigned> ports(unsigned C, bool With512) const {
    return With512 && !ClassPorts512[C].empty() ? ArrayRef<unsigned>(ClassPorts512[C])
                                                : ArrayRef<unsigned>(ClassPorts[C]);
  }
  /// Whether any class lists its ports; the region resource bound is then
  /// taken over port sets rather than per class.
  bool hasPortSets() const { return HasPortSets; }
  unsigned dispatchWidth() const { return DispatchWidth; }
  /// Width in bits of the vector unit the probes were measured on
  /// (resources.json "vector_width_bits": 128, 256 or 512; 128 when absent).
  unsigned vectorWidthBits() const { return VectorWidthBits; }

  /// Cost of an IR operation form, "<op> <type>" for scalar operations and
  /// "sv.<op> v4i32" for lane-0 vector operations; the sequence entry for the
  /// key in ops.json when one exists (measured whole), else the sum over the
  /// machine opcodes listed for it in probes.json.
  std::optional<OpCost> irOpCost(StringRef ProbeKey) const;
  /// Cost of a transfer, or nullopt when transfers.json has no such key. The
  /// five Part-2 kinds are required keys; LaneExtract and LaneInsert
  /// ("lane_extract", "lane_insert") are optional: a directory without them
  /// cannot cost Scalarize boundaries.
  std::optional<OpCost> transferCost(TransferKind K) const;
  /// The whole-vector lane transfer of vector type \p VTy (a typeKey, "v8i32"):
  /// lane_transfers.json "lane_extract <VTy>" / "lane_insert <VTy>", the
  /// opcode sequence the backend emits to move every lane to / from general
  /// registers. Resource use and micro-ops are summed over its "opcodes"; the
  /// latency is the sum over its "path", the critical chain of the sequence
  /// (both read from the file; the pass knows no opcode names). Nullopt for
  /// \p K other than LaneExtract / LaneInsert, or when the entry is absent.
  std::optional<OpCost> laneTransferCost(TransferKind K, StringRef VTy) const;
  /// The concurrency table of a sequence entry, or null.
  const ConcurrencyTable *concurrency(StringRef ProbeKey) const;
  /// Instruction-window size in micro-ops (resources.json "window_uops"), or 0
  /// when absent.
  unsigned windowUops() const { return WindowUops; }

private:
  PortDescription() = default;

  SmallVector<std::string, 8> ClassNames;
  SmallVector<unsigned, 8> ClassUnits;
  SmallVector<SmallVector<unsigned, 8>, 8> ClassPorts, ClassPorts512;
  bool HasPortSets = false;
  unsigned DispatchWidth = 0;
  unsigned VectorWidthBits = 128;
  unsigned WindowUops = 0;
  StringMap<ConcurrencyTable> Concurrency;
  StringMap<OpCost> Probes;
  std::optional<OpCost> Transfers[NumTransferKinds];
  /// Per-width lane transfers (lane_transfers.json), keyed "lane_extract <vty>" / "lane_insert <vty>".
  StringMap<OpCost> LaneTransfers;
};

/// The probe-key spelling of \p T: "i32", "i64", "v<N>i32" or "v<N>i64"; empty
/// for any other type.
std::string typeKey(Type *T);

/// Lanes (of i32) that one floating-point divide sequence handles together:
/// min(the port-info vector width, the target's fixed vector register width)
/// / 64, at least 2.
unsigned fpDivideGroup(const PortDescription &PD,
                       const TargetTransformInfo &TTI);

/// Port-info directory name for a target triple: "AArch64", "x86" or "".
std::string archDirFor(const Triple &T);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_PORTDESCRIPTION_H
