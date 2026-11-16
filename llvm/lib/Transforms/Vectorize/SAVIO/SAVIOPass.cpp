//===- SAVIOPass.cpp - Scalar/vector instruction orchestration ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/SAVIOPass.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include <map>

using namespace llvm;
using namespace llvm::savio;

#define DEBUG_TYPE "savio"

cl::opt<bool> llvm::EnableSAVIO(
    "enable-savio", cl::init(false), cl::Hidden,
    cl::desc("Run the SAVIO scalar/vector orchestration pass at the end of "
             "the optimizer"));

cl::opt<std::string>
    llvm::SAVIOPortInfo("savio-port-info", cl::init(""), cl::Hidden,
                        cl::desc("Root directory of SAVIO port-info files"));

cl::opt<std::string>
    llvm::SAVIOCPU("savio-cpu", cl::init(""), cl::Hidden,
                   cl::desc("Override the CPU used to select SAVIO port-info"));

static cl::opt<double> SAVIOColdFraction(
    "savio-cold-fraction", cl::init(0.01), cl::Hidden,
    cl::desc("Skip regions whose block frequency is below this fraction of "
             "the entry block's"));

namespace {

/// Outcome of loading the port-info of one (root, arch, CPU): a description,
/// or the reason there is none.
struct LoadResult {
  std::shared_ptr<const PortDescription> PD;
  bool Missing = false; // The CPU has no port-info directory.
  std::string Error;    // Malformed port-info, naming file and key.
};

/// Loads each (root, arch, CPU) once and caches the outcome, so a module with
/// many functions reads the files once.
LoadResult loadPortInfo(StringRef Root, StringRef Arch, StringRef CPU) {
  static sys::Mutex Lock;
  static std::map<std::string, LoadResult> Cache;
  std::string Key = (Root + "\n" + Arch + "\n" + CPU).str();
  sys::ScopedLock Guard(Lock);
  auto It = Cache.find(Key);
  if (It != Cache.end())
    return It->second;

  LoadResult R;
  if (Root.empty() || Arch.empty() || CPU.empty()) {
    R.Missing = true;
  } else if (Expected<std::unique_ptr<PortDescription>> PD =
                 PortDescription::load(Root, Arch, CPU)) {
    R.PD = std::move(*PD);
  } else {
    handleAllErrors(PD.takeError(), [&](const ErrorInfoBase &E) {
      if (E.convertToErrorCode() ==
          std::make_error_code(std::errc::no_such_file_or_directory))
        R.Missing = true;
      else
        R.Error = E.message();
    });
  }
  return Cache.try_emplace(Key, std::move(R)).first->second;
}

} // namespace

PreservedAnalyses SAVIOPass::run(Function &F, FunctionAnalysisManager &FAM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  std::string CPU = SAVIOCPU;
  if (CPU.empty())
    CPU = F.getFnAttribute("target-cpu").getValueAsString().str();
  std::string Arch = archDirFor(F.getParent()->getTargetTriple());

  LoadResult R = loadPortInfo(SAVIOPortInfo, Arch, CPU);
  auto &ORE = FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  if (!R.PD) {
    // Nothing to model against: change nothing, say why once per function.
    ORE.emit([&] {
      if (R.Missing)
        return OptimizationRemarkMissed(DEBUG_TYPE, "NoPortInfo", &F)
               << "no port-info for CPU '" << CPU << "'";
      return OptimizationRemarkMissed(DEBUG_TYPE, "PortInfoError", &F)
             << "invalid port-info for CPU '" << CPU << "': " << R.Error;
    });
    return PreservedAnalyses::all();
  }

  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &BFI = FAM.getResult<BlockFrequencyAnalysis>(F);
  double EntryFreq = BFI.getBlockFreq(&F.getEntryBlock()).getFrequency();

  auto BoundName = [](Bound B) {
    switch (B) {
    case Bound::Resource:
      return "resource";
    case Bound::Recurrence:
      return "recurrence";
    case Bound::Dispatch:
      return "dispatch";
    }
    llvm_unreachable("unknown bound");
  };

  // Decisions only: the IR is not rewritten yet.
  for (Region &Reg : formRegions(F, LI)) {
    if (BFI.getBlockFreq(Reg.BB).getFrequency() <
        SAVIOColdFraction * EntryFreq)
      continue;
    Decision D = decide(Reg, *R.PD, SAVIOMargin);
    std::string Before = formatv("{0:F2}", D.Before.t()).str();
    if (D.Promoted.empty()) {
      ORE.emit([&] {
        return OptimizationRemarkMissed(DEBUG_TYPE, "NotProfitable",
                                        Reg.Groups[0].Ops[0])
               << "T " << Before << " ("
               << BoundName(D.Before.limiting()) << ")";
      });
      continue;
    }
    std::string After = formatv("{0:F2}", D.After.t()).str();
    for (unsigned G : D.Promoted)
      ORE.emit([&] {
        return OptimizationRemark(DEBUG_TYPE, "Promoted",
                                  Reg.Groups[G].Ops[0])
               << "T " << Before << " -> " << After << " ("
               << BoundName(D.Before.limiting()) << ")";
      });
  }
  return PreservedAnalyses::all();
}
