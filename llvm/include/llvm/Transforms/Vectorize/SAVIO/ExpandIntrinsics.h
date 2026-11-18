//===- ExpandIntrinsics.h - savio-expand: lower llvm.savio.* to IR -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Replaces each llvm.savio.sv.* / llvm.savio.fpdiv<L>.* call by plain IR that
// has the same defined lanes. Targets that select an intrinsic themselves
// (AArch64 keeps all of them) do not run the pass; a target that selects only
// some passes a Keep predicate that returns true for those.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_EXPANDINTRINSICS_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_EXPANDINTRINSICS_H

#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"
#include <functional>

namespace llvm {

class FunctionPass;
class Module;
class PassRegistry;

/// Intrinsics for which \p Keep returns true are left alone.
using SAVIOKeepFn = std::function<bool(Intrinsic::ID)>;

class SAVIOExpandIntrinsicsPass
    : public PassInfoMixin<SAVIOExpandIntrinsicsPass> {
  SAVIOKeepFn Keep;
  // Module-level "declares an llvm.savio.* function" scan, cached per module.
  const Module *CachedModule = nullptr;
  bool HasSAVIO = true;

public:
  SAVIOExpandIntrinsicsPass(SAVIOKeepFn Keep = nullptr)
      : Keep(std::move(Keep)) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

LLVM_ABI void initializeSAVIOExpandIntrinsicsLegacyPassPass(PassRegistry &);
/// Legacy-pass-manager form. (std::function rather than function_ref: the pass
/// outlives the call, so it must own its predicate.)
LLVM_ABI FunctionPass *
createSAVIOExpandIntrinsicsPass(SAVIOKeepFn Keep = nullptr);

} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_EXPANDINTRINSICS_H
