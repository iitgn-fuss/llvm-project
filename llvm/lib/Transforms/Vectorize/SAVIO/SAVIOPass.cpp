//===- SAVIOPass.cpp - Scalar/vector instruction orchestration ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/SAVIOPass.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "savio"

cl::opt<bool> EnableSAVIO("enable-savio", cl::init(false), cl::Hidden,
                          cl::desc("Run the SAVIO scalar/vector orchestration "
                                   "pass at the end of the optimizer"));

cl::opt<std::string>
    SAVIOPortInfo("savio-port-info", cl::init(""), cl::Hidden,
                  cl::desc("Root directory of SAVIO port-info files"));

cl::opt<std::string>
    SAVIOCPU("savio-cpu", cl::init(""), cl::Hidden,
             cl::desc("Override the CPU used to select SAVIO port-info"));

PreservedAnalyses SAVIOPass::run(Function &F, FunctionAnalysisManager &FAM) {
  return PreservedAnalyses::all();
}
