//===- SAVIOPipeline.h - SAVIO-IR pipeline hook -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Registers SAVIO-IR at the end of the optimization pipeline. Target machines
// call this from registerPassBuilderCallbacks. Header-inline so that
// LLVMVectorize does not need to link LLVMPasses.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOPIPELINE_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOPIPELINE_H

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Vectorize/SAVIO/SAVIOIR.h"

namespace llvm {

inline void registerSAVIOIRCallbacks(PassBuilder &PB) {
  PB.registerOptimizerLastEPCallback(
      [](ModulePassManager &MPM, OptimizationLevel Level,
         ThinOrFullLTOPhase Phase) {
        // Run once, in the final optimization pipeline.
        if (Phase == ThinOrFullLTOPhase::ThinLTOPreLink ||
            Phase == ThinOrFullLTOPhase::FullLTOPreLink)
          return;
        if (EnableSAVIO && Level != OptimizationLevel::O0)
          MPM.addPass(createModuleToFunctionPassAdaptor(SAVIOIRPass()));
      });
}

} // namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOPIPELINE_H
