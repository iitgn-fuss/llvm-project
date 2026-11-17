//===- SAVIOPass.cpp - Scalar/vector instruction orchestration ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/SAVIOPass.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/Transforms/Vectorize/SAVIO/SingletonVectorize.h"
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

static cl::opt<bool> SAVIONoUnroll(
    "savio-no-unroll", cl::init(false), cl::Hidden,
    cl::desc("Never unroll a loop to reach a fraction (decide on the loop as "
             "it is)"));

static cl::opt<double> SAVIORedecideMargin(
    "savio-test-redecide-margin", cl::init(-1), cl::Hidden,
    cl::desc("Margin for the decision on a loop SAVIO has just unrolled "
             "(negative: the usual margin; for tests of the re-decision)"));

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
  bool First = false;   // Returned by the call that did the loading.
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
  Cache.try_emplace(Key, R);
  R.First = true;
  return R;
}

/// Most instructions in a loop body SAVIO will unroll.
constexpr unsigned MaxUnrollBodySize = 64;

/// True if SAVIO may unroll the single-block innermost loop \p L: a small
/// body, no explicit unroll request or prohibition in its metadata, and a trip
/// count ScalarEvolution can express (a loop that runs at most 8 times is left
/// alone: unrolling it would be full unrolling).
bool mayUnroll(const Loop &L, ScalarEvolution &SE) {
  if (!L.isInnermost() || L.getNumBlocks() != 1 ||
      L.getHeader()->size() > MaxUnrollBodySize)
    return false;
  for (StringRef Name :
       {"llvm.loop.unroll.disable", "llvm.loop.unroll.runtime.disable",
        "llvm.loop.unroll.full", "llvm.loop.unroll.enable"})
    if (getBooleanLoopAttribute(&L, Name))
      return false;
  if (findStringMetadataForLoop(&L, "llvm.loop.unroll.count"))
    return false;
  if (!SE.hasLoopInvariantBackedgeTakenCount(&L))
    return false;
  // The unroller lowers the count to the maximum trip count and unrolls fully
  // when the loop cannot run more often than the count: leave such loops
  // alone. (A full unroll that still happens is handled by the caller.)
  unsigned TC = SE.getSmallConstantTripCount(&L);
  unsigned MaxTC = SE.getSmallConstantMaxTripCount(&L);
  if (TC != 0 && TC <= 8)
    return false;
  if (MaxTC != 0 && MaxTC <= 8)
    return false;
  // The scalar remainder of a loop the vectorizer already vectorized runs
  // fewer than VF times: unrolling it only adds dead code.
  if (getBooleanLoopAttribute(&L, "llvm.loop.isvectorized") ||
      findStringMetadataForLoop(&L, "llvm.loop.vectorize.epilogue") ||
      findStringMetadataForLoop(&L, "llvm.loop.isvectorized"))
    return false;
  return true;
}

/// Unrolls \p L by \p Count with a remainder loop that keeps any trip count
/// correct. Unmodified, with the IR changed at most by the loop-simplify and
/// LCSSA canonicalization, if the loop cannot be unrolled; FullyUnrolled if the
/// unroller removed the loop (the Loop and its phis are then gone).
LoopUnrollResult unrollWithRemainder(Loop *L, unsigned Count, LoopInfo &LI,
                         ScalarEvolution &SE, DominatorTree &DT,
                         AssumptionCache &AC, TargetTransformInfo &TTI,
                         OptimizationRemarkEmitter &ORE) {
  // LCSSA first: simplifyLoop below preserves it, so it must hold. The unroller
  // keeps LCSSA only of a nest that has it: the whole nest.
  Loop *Outermost = L;
  while (Outermost->getParentLoop())
    Outermost = Outermost->getParentLoop();
  if (!Outermost->isRecursivelyLCSSAForm(DT, LI))
    formLCSSARecursively(*Outermost, DT, &LI, &SE);
  if (!L->isLoopSimplifyForm())
    simplifyLoop(L, &DT, &LI, &SE, &AC, nullptr, /*PreserveLCSSA=*/true);
  if (!L->isLoopSimplifyForm())
    return LoopUnrollResult::Unmodified;
  UnrollLoopOptions ULO;
  ULO.Count = Count;
  ULO.Force = false; // Never without the remainder loop.
  ULO.Runtime = true;
  ULO.AllowExpensiveTripCount = false;
  ULO.UnrollRemainder = false;
  ULO.ForgetAllSCEV = false;
  ULO.Heart = getLoopConvergenceHeart(L);
  ULO.SCEVExpansionBudget = SCEVCheapExpansionBudget;
  return UnrollLoop(L, ULO, &LI, &SE, &DT, &AC, &TTI, &ORE,
                    /*PreserveLCSSA=*/true);
}

} // namespace

PreservedAnalyses SAVIOPass::run(Function &F, FunctionAnalysisManager &FAM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  // The sv intrinsics are unsafe or unselectable here: optnone code must not be
  // changed, the udiv/urem expansion raises FP exception flags (strictfp), and
  // without vector registers the intrinsics cannot be selected.
  if (F.hasOptNone() || F.hasFnAttribute(Attribute::StrictFP)) {
    LLVM_DEBUG(dbgs() << "savio: skipping " << F.getName()
                      << ": optnone or strictfp\n");
    return PreservedAnalyses::all();
  }
  {
    auto &TTI = FAM.getResult<TargetIRAnalysis>(F);
    Type *VecTy = FixedVectorType::get(Type::getInt32Ty(F.getContext()), 4);
    if (TTI.getNumberOfRegisters(TTI.getRegisterClassForType(true, VecTy)) ==
        0) {
      LLVM_DEBUG(dbgs() << "savio: skipping " << F.getName()
                        << ": no vector registers\n");
      return PreservedAnalyses::all();
    }
  }

  std::string CPU = SAVIOCPU;
  if (CPU.empty())
    CPU = F.getFnAttribute("target-cpu").getValueAsString().str();
  std::string Arch = archDirFor(F.getParent()->getTargetTriple());

  LoadResult R = loadPortInfo(SAVIOPortInfo, Arch, CPU);
  auto &ORE = FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  if (!R.PD) {
    // Nothing to model against: change nothing. A malformed directory is a
    // user error, so it is also a warning, once per (root, arch, CPU).
    if (!R.Error.empty() && R.First)
      F.getContext().diagnose(DiagnosticInfoGeneric(
          "savio: invalid port-info for CPU '" + CPU + "': " + R.Error,
          DS_Warning));
    // Say why once per function.
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

  // Instructions without a port-info entry: TTI latency (1 if unknown) and one
  // micro-op unless TTI says the instruction is free (folded casts, phis,
  // debug intrinsics); no resource use.
  auto &TTI = FAM.getResult<TargetIRAnalysis>(F);
  auto Fallback = [&](const Instruction &I) {
    OpCost C;
    InstructionCost Lat =
        TTI.getInstructionCost(&I, TargetTransformInfo::TCK_Latency);
    C.Latency = Lat.isValid() ? std::max<int64_t>(Lat.getValue(), 0) : 1;
    InstructionCost Size =
        TTI.getInstructionCost(&I, TargetTransformInfo::TCK_SizeAndLatency);
    C.Uops = Size.isValid() && Size.getValue() == 0 ? 0 : 1;
    return C;
  };

  // The unroller needs these; fetched only if a loop is a candidate.
  bool Changed = false;
  // k/U of the moved classes ("none" if nothing moves) and T per original
  // iteration, as the remarks print them.
  auto Fraction = [&](const Decision &D) {
    std::string S;
    for (const ClassMove &M : D.MovedPerClass)
      S += (S.empty() ? "" : "+") + utostr(M.K) + "/" + utostr(M.U);
    return S.empty() ? std::string("none") : S;
  };
  auto Time = [&](const Decision &D, unsigned Unrolled) {
    return formatv("{0:F2}", D.After.t() / Unrolled).str();
  };
  // Applies the decision \p D made on the region as it is. \p UnrolledBy is
  // the factor SAVIO unrolled the loop by (1 if it did not), and \p Est the
  // decision on the virtually unrolled loop that led to it: the decision on the
  // real loop can differ, and then the remark says both.
  auto Apply = [&](Region &Reg, const Decision &D, unsigned UnrolledBy,
                   const Decision *Est = nullptr) {
    // T per original iteration: the unrolled loop's T covers UnrolledBy of
    // them.
    std::string Before = formatv("{0:F2}", D.Before.t() / UnrolledBy).str();
    std::string Unrolled;
    if (UnrolledBy > 1) {
      Unrolled = ", unrolled by " + utostr(UnrolledBy);
      if (Est) {
        std::string E = Fraction(*Est) + " T " + Time(*Est, 1);
        std::string A = D.Promoted.empty()
                            ? std::string("none")
                            : Fraction(D) + " T " + Time(D, UnrolledBy);
        Unrolled += " (estimated " + E;
        if (A != E)
          Unrolled += ", applied " + A;
        Unrolled += ")";
      }
    }
    if (D.Promoted.empty()) {
      ORE.emit([&] {
        return OptimizationRemarkMissed(DEBUG_TYPE, "NotProfitable",
                                        Reg.Groups[0].Ops[0])
               << "T " << Before << " (" << BoundName(D.Before.limiting())
               << ")" << Unrolled;
      });
      return;
    }
    std::string After = formatv("{0:F2}", D.After.t() / UnrolledBy).str();
    for (unsigned G : D.Promoted) {
      unsigned Cls = Reg.Groups[G].SliceClass, K = 0, U = 0;
      for (const ClassMove &M : D.MovedPerClass)
        if (M.Class == Cls) {
          K = M.K;
          U = M.U;
        }
      // The instances of a vector divide are lane pairs; the remark counts
      // lanes.
      if (Reg.Groups[G].VectorDivide) {
        K *= 2;
        U *= 2;
      }
      ORE.emit([&] {
        // Singleton Vectorize keeps the name "Promoted"; the opposite
        // direction is "Scalarized".
        OptimizationRemark Rm(
            DEBUG_TYPE,
            Reg.Groups[G].Dir == Direction::ToScalar ? "Scalarized"
                                                     : "Promoted",
            Reg.Groups[G].Ops[0]);
        Rm << "T " << Before << " -> " << After << " ("
           << BoundName(D.Before.limiting()) << "), " << utostr(K) << "/"
           << utostr(U) << " of slice " << utostr(Cls);
        Rm << Unrolled;
        return Rm;
      });
    }
    // The remarks above name operations the rewrite erases. The lane pairs of
    // one vector divide are rewritten together.
    SmallVector<std::pair<Instruction *, unsigned>, 2> Divides;
    for (unsigned G : D.Promoted) {
      const Group &Grp = Reg.Groups[G];
      // The Scalarize rewrite does not exist yet: a decided Scalarize group is
      // reported and the IR is left as it is.
      if (Grp.Dir == Direction::ToScalar)
        continue;
      if (!Grp.VectorDivide) {
        rewriteSingletonVectorize(Grp, Reg.L);
        continue;
      }
      auto *It =
          find_if(Divides, [&](auto &P) { return P.first == Grp.Ops[0]; });
      if (It == Divides.end())
        It = Divides.insert(Divides.end(), {Grp.Ops[0], 0u});
      It->second |= 1u << Grp.Pair;
    }
    for (auto &[I, Mask] : Divides)
      rewriteVectorDivide(I, Mask);
    Changed |= any_of(D.Promoted, [&](unsigned G) {
      return Reg.Groups[G].Dir != Direction::ToScalar;
    });
  };

  bool Forced = SAVIOForce || SAVIOForceCount >= 0 || SAVIONoUnroll;
  for (Region &Reg : formRegions(F, LI)) {
    if (BFI.getBlockFreq(Reg.BB).getFrequency() <
        SAVIOColdFraction * EntryFreq)
      continue;
    // Unrolling is only for the cost-driven search; the force flags act on
    // the region as it is.
    ScalarEvolution *SE = nullptr;
    bool CanUnroll = false;
    if (!Forced && Reg.L) {
      SE = &FAM.getResult<ScalarEvolutionAnalysis>(F);
      CanUnroll = mayUnroll(*Reg.L, *SE);
      // Until the Scalarize rewrite exists a Scalarize decision is only
      // reported: do not unroll a loop for the fraction it would reach.
      if (any_of(Reg.Groups, [](const Group &G) {
            return G.Dir == Direction::ToScalar;
          }))
        CanUnroll = false;
    }
    Decision D = decide(Reg, *R.PD, Fallback, SAVIOMargin, CanUnroll);
    if (D.Unroll > 1) {
      // Unroll to the decided factor, form the regions of the unrolled loop
      // again and decide on it as it is now: its copies of a slice are
      // instances of one class. If the loop cannot be unrolled, decide again
      // without it.
      auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
      auto &AC = FAM.getResult<AssumptionAnalysis>(F);
      BasicBlock *Header = Reg.BB;
      Changed = true;
      LoopUnrollResult UR =
          unrollWithRemainder(Reg.L, D.Unroll, LI, *SE, DT, AC, TTI, ORE);
      // A full unroll deleted the loop and this region's instructions: there
      // is nothing left to decide on.
      if (UR == LoopUnrollResult::FullyUnrolled)
        continue;
      if (UR == LoopUnrollResult::PartiallyUnrolled) {
        for (Region &New : formRegions(F, LI))
          if (New.BB == Header) {
            Apply(New,
                  decide(New, *R.PD, Fallback,
                         SAVIORedecideMargin >= 0 ? SAVIORedecideMargin
                                                  : SAVIOMargin,
                         false),
                  D.Unroll, &D);
            break;
          }
        continue;
      }
      D = decide(Reg, *R.PD, Fallback, SAVIOMargin, false);
    }
    Apply(Reg, D, 1);
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
