//===- SAVIOIR.h - SAVIO-IR: scalar/vector orchestration pass ---*- C++ -*-===//
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

#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOIR_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOIR_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include <string>

namespace llvm {

extern cl::opt<bool> EnableSAVIO;
extern cl::opt<std::string> SAVIOPortInfo;
extern cl::opt<std::string> SAVIOCPU;

class SAVIOIRPass : public PassInfoMixin<SAVIOIRPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_SAVIO_SAVIOIR_H
