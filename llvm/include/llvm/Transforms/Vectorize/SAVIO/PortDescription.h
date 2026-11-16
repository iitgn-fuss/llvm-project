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

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <optional>
#include <string>

namespace llvm {
class Triple;

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
  LaneToLane0
};

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
  unsigned dispatchWidth() const { return DispatchWidth; }

  /// Cost of an IR operation form, "<op> <type>" for scalar operations and
  /// "sv.<op> v4i32" for lane-0 vector operations; the sequence entry for the
  /// key in ops.json when one exists (measured whole), else the sum over the
  /// machine opcodes listed for it in probes.json.
  std::optional<OpCost> irOpCost(StringRef ProbeKey) const;
  std::optional<OpCost> transferCost(TransferKind K) const;
  /// The concurrency table of a sequence entry, or null.
  const ConcurrencyTable *concurrency(StringRef ProbeKey) const;
  /// Instruction-window size in micro-ops (resources.json "window_uops"), or 0
  /// when absent.
  unsigned windowUops() const { return WindowUops; }

private:
  PortDescription() = default;

  SmallVector<std::string, 8> ClassNames;
  SmallVector<unsigned, 8> ClassUnits;
  unsigned DispatchWidth = 0;
  unsigned WindowUops = 0;
  StringMap<ConcurrencyTable> Concurrency;
  StringMap<OpCost> Probes;
  std::optional<OpCost> Transfers[5];
};

/// Port-info directory name for a target triple: "AArch64" or "".
std::string archDirFor(const Triple &T);

} // end namespace savio
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_PORTDESCRIPTION_H
