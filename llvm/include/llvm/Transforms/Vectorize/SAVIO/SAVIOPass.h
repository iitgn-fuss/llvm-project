//===- SAVIOPass.h - Scalar/vector instruction orchestration ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SAVIO moves selected scalar integer operations onto vector units (Singleton
// Vectorize) and back (Scalarize) when a port-pressure model says it is
// profitable. The pass is off unless -enable-savio is given.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOPASS_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOPASS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class SAVIOPass : public PassInfoMixin<SAVIOPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOPASS_H
