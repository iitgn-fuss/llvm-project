//===- SAVIOIR.cpp - SAVIO-IR: scalar/vector orchestration pass -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/SAVIOIR.h"
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
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/Transforms/Vectorize/SAVIO/Scalarize.h"
#include "llvm/Transforms/Vectorize/SAVIO/SingletonVectorize.h"
#include <map>
#include <tuple>

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

static cl::opt<double> SAVIORemainderFraction(
    "savio-remainder-fraction", cl::init(0.50), cl::Hidden,
    cl::desc("Minimum entry-block frequency for a loop, straight-line, or "
             "remainder region. Cold regions avoid code growth from SAVIO"));

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
static cl::opt<unsigned> SAVIOMaxUnrollBodySize(
    "savio-max-unroll-body-size", cl::init(128), cl::Hidden,
    cl::desc("Maximum instruction count in a loop body SAVIO will unroll"));

/// True if the loop header already contains a fixed-width integer vector
/// operation that Scalarize can split.  Loop metadata alone is not enough:
/// LLVM can mark a scalar loop remainder as isvectorized even though it contains
/// no vector body.  In that case unrolling would only duplicate scalar work.
static bool hasVectorCandidateBody(const BasicBlock &BB) {
  for (const Instruction &I : BB) {
    auto *VT = dyn_cast<FixedVectorType>(I.getType());
    if (!VT || !(VT->getElementType()->isIntegerTy(32) ||
                 VT->getElementType()->isIntegerTy(64)))
      continue;
    unsigned Bits =
        VT->getPrimitiveSizeInBits().getFixedValue();
    if (Bits != 128 && Bits != 256 && Bits != 512)
      continue;
    switch (I.getOpcode()) {
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::Mul:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
      return true;
    default:
      break;
    }
  }
  return false;
}

/// True if SAVIO may unroll the single-block innermost loop \p L: a small
/// body, no explicit unroll request or prohibition in its metadata, and a trip
/// count ScalarEvolution can express (a loop that runs at most 8 times is left
/// alone: unrolling it would be full unrolling).
bool mayUnroll(const Loop &L, ScalarEvolution &SE) {
  if (!L.isInnermost() || L.getNumBlocks() != 1 ||
      L.getHeader()->size() > SAVIOMaxUnrollBodySize)
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
  if ((getBooleanLoopAttribute(&L, "llvm.loop.isvectorized") ||
       findStringMetadataForLoop(&L, "llvm.loop.vectorize.epilogue") ||
       findStringMetadataForLoop(&L, "llvm.loop.isvectorized")) &&
      !hasVectorCandidateBody(*L.getHeader()))
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

// 64-element de Bruijn table (Terriberry: 817.flac_s) with multiplier 0x0218A392CD3D5DBFULL.
static const uint8_t DeBruijnTable64_Terriberry[64] = {
    0,  1,  2,  7,  3, 13,  8, 19,  4, 25, 14, 28,  9, 34, 20, 40,
    5, 17, 26, 38, 15, 46, 29, 48, 10, 31, 35, 54, 21, 50, 41, 57,
   63,  6, 12, 18, 24, 27, 33, 39, 16, 37, 45, 47, 30, 53, 49, 56,
   62, 11, 23, 32, 36, 44, 52, 55, 61, 22, 43, 51, 60, 42, 59, 58
};

// 64-element de Bruijn table (SEAL / Boost: 750.sealcrypto_r, 766.femflow_r) with multiplier 0x07EDD5E59A4E28C2ULL.
static const uint8_t DeBruijnTable64_SEAL[64] = {
   63,  0, 58,  1, 59, 47, 53,  2, 60, 39, 48, 27, 54, 33, 42,  3,
   61, 51, 37, 40, 49, 18, 28, 20, 55, 30, 34, 11, 43, 14, 22,  4,
   62, 57, 46, 52, 38, 26, 32, 41, 50, 36, 17, 19, 29, 10, 13, 21,
   56, 45, 25, 31, 35, 16,  9, 12, 44, 24, 15,  8, 23,  7,  6,  5
};

// 64-element de Bruijn table (Psnip: 838.diamond_s) with multiplier 0x03F79D71B4CB0A89ULL.
static const uint8_t DeBruijnTable64_Psnip[64] = {
    0, 47,  1, 56, 48, 27,  2, 60, 57, 49, 41, 37, 28, 16,  3, 61,
   54, 58, 35, 52, 50, 42, 21, 44, 38, 32, 29, 23, 17, 11,  4, 62,
   46, 55, 26, 59, 40, 36, 15, 53, 34, 51, 20, 43, 31, 22, 10, 45,
   25, 39, 14, 33, 19, 30,  9, 24, 13, 18,  8, 12,  7,  6,  5, 63
};

// 32-element de Bruijn table (Eigen / Boost / Zstd / Psnip) with multiplier 0x07C4ACDDU.
static const uint8_t DeBruijnTable32_Clz[32] = {
    0,  9,  1, 10, 13, 21,  2, 29, 11, 14, 16, 18, 22, 25,  3, 30,
    8, 12, 20, 28, 15, 17, 24,  7, 19, 27, 23,  6, 26,  5,  4, 31
};

static bool matchTableData(const Constant *Init, ArrayRef<uint8_t> Expected) {
  if (auto *CDS = dyn_cast<ConstantDataSequential>(Init)) {
    if (CDS->getNumElements() != Expected.size())
      return false;
    for (unsigned i = 0; i < Expected.size(); ++i) {
      if (CDS->getElementAsInteger(i) != Expected[i])
        return false;
    }
    return true;
  }
  if (auto *CA = dyn_cast<ConstantArray>(Init)) {
    if (CA->getNumOperands() != Expected.size())
      return false;
    for (unsigned i = 0; i < Expected.size(); ++i) {
      auto *CI = dyn_cast<ConstantInt>(CA->getOperand(i));
      if (!CI || CI->getZExtValue() != Expected[i])
        return false;
    }
    return true;
  }
  return false;
}

enum class DeBruijnKind { None, Terriberry64, SEAL64, Psnip64, Clz32 };

static DeBruijnKind identifyDeBruijnTable(const Value *Ptr) {
  const Value *Stripped = Ptr->stripPointerCasts();
  auto *GV = dyn_cast<GlobalVariable>(Stripped);
  if (!GV || !GV->hasDefinitiveInitializer())
    return DeBruijnKind::None;
  const Constant *Init = GV->getInitializer();
  if (matchTableData(Init, DeBruijnTable64_Terriberry))
    return DeBruijnKind::Terriberry64;
  if (matchTableData(Init, DeBruijnTable64_SEAL))
    return DeBruijnKind::SEAL64;
  if (matchTableData(Init, DeBruijnTable64_Psnip))
    return DeBruijnKind::Psnip64;
  if (matchTableData(Init, DeBruijnTable32_Clz))
    return DeBruijnKind::Clz32;
  return DeBruijnKind::None;
}

static Value *peelSmear(Value *Val, ArrayRef<unsigned> Shifts) {
  using namespace llvm::PatternMatch;
  auto matchSmearStep = [](Value *V, unsigned ShiftAmt) -> Value * {
    Value *Base = nullptr, *Shifted = nullptr;
    if (match(V, m_c_Or(m_Value(Base), m_Value(Shifted)))) {
      if (match(Shifted, m_LShr(m_Specific(Base), m_SpecificInt(ShiftAmt))))
        return Base;
      if (match(Base, m_LShr(m_Specific(Shifted), m_SpecificInt(ShiftAmt))))
        return Shifted;
    }
    return nullptr;
  };

  Value *Cur = Val;
  for (unsigned S : Shifts) {
    if (Value *Next = matchSmearStep(Cur, S))
      Cur = Next;
  }
  return Cur;
}

static Value *extractBaseV(Value *E) {
  using namespace llvm::PatternMatch;
  Value *V = nullptr;
  Value *Op0 = nullptr, *Op1 = nullptr;
  if (match(E, m_c_Or(m_Value(Op0), m_Value(Op1)))) {
    if (match(Op1, m_LShr(m_Specific(Op0), m_SpecificInt(1)))) {
      V = Op0;
    } else if (match(Op0, m_LShr(m_Specific(Op1), m_SpecificInt(1)))) {
      V = Op1;
    } else {
      Value *Base0 = nullptr, *Base1 = nullptr;
      const APInt *Shift0 = nullptr, *Shift1 = nullptr;
      if (match(Op0, m_LShr(m_Value(Base0), m_APInt(Shift0))) &&
          match(Op1, m_LShr(m_Value(Base1), m_APInt(Shift1))) &&
          Base0 == Base1) {
        if (*Shift1 == *Shift0 + 1)
          V = Op0;
        else if (*Shift0 == *Shift1 + 1)
          V = Op1;
      }
    }
  }
  return V ? V : E;
}

struct DeBruijnMatch {
  LoadInst *LI;
  Value *V;
  unsigned BitWidth;
  uint64_t ZeroVal;
};

static std::optional<DeBruijnMatch> matchDeBruijn(LoadInst *LI) {
  auto *GEP = dyn_cast<GEPOperator>(LI->getPointerOperand());
  if (!GEP || GEP->getNumOperands() < 2)
    return std::nullopt;

  DeBruijnKind Kind = identifyDeBruijnTable(GEP->getPointerOperand());
  if (Kind == DeBruijnKind::None)
    return std::nullopt;

  Value *Idx = GEP->getOperand(GEP->getNumOperands() - 1);
  using namespace llvm::PatternMatch;
  Value *SubIdx = nullptr;
  while (match(Idx, m_ZExt(m_Value(SubIdx))) ||
         match(Idx, m_SExt(m_Value(SubIdx))) ||
         match(Idx, m_And(m_Value(SubIdx), m_SpecificInt(63))) ||
         match(Idx, m_And(m_Value(SubIdx), m_SpecificInt(31)))) {
    Idx = SubIdx;
  }

  if (Kind == DeBruijnKind::Clz32) {
    Value *M = nullptr;
    if (!match(Idx, m_LShr(m_Value(M), m_SpecificInt(27))))
      return std::nullopt;
    Value *Smeared = nullptr;
    constexpr uint32_t Clz32Mult = 0x07C4ACDDU;
    if (!match(M, m_Mul(m_Value(Smeared), m_SpecificInt(Clz32Mult))))
      return std::nullopt;
    Value *Peeled = peelSmear(Smeared, {16, 8, 4, 2});
    Value *V = extractBaseV(Peeled);
    if (!V || !V->getType()->isIntegerTy(32))
      return std::nullopt;
    return DeBruijnMatch{LI, V, 32, 0};
  }

  // 64-bit tables
  Value *M = nullptr;
  if (!match(Idx, m_LShr(m_Value(M), m_SpecificInt(58))))
    return std::nullopt;

  if (Kind == DeBruijnKind::Terriberry64) {
    constexpr uint64_t TerriberryMult = 0x0218A392CD3D5DBFULL;
    Value *Y = nullptr;
    if (match(M, m_c_Add(m_Mul(m_Value(Y), m_SpecificInt(TerriberryMult)),
                         m_SpecificInt(TerriberryMult)))) {
    } else if (match(M, m_Mul(m_c_Add(m_Value(Y), m_SpecificInt(1)),
                              m_SpecificInt(TerriberryMult)))) {
    } else if (match(M, m_Mul(m_Value(Y), m_SpecificInt(TerriberryMult)))) {
      Value *Y2 = nullptr;
      if (match(Y, m_c_Add(m_Value(Y2), m_SpecificInt(1))))
        Y = Y2;
    } else {
      return std::nullopt;
    }

    Value *A = nullptr;
    Value *Sh1 = nullptr, *Sh33 = nullptr;
    if (match(Y, m_c_Or(m_Value(Sh1), m_Value(Sh33)))) {
      if (match(Sh1, m_LShr(m_Value(A), m_SpecificInt(1))) &&
          match(Sh33, m_LShr(m_Specific(A), m_SpecificInt(33)))) {
      } else if (match(Sh1, m_LShr(m_Value(A), m_SpecificInt(33))) &&
                 match(Sh33, m_LShr(m_Specific(A), m_SpecificInt(1)))) {
      }
    }
    if (!A) {
      Value *Smeared32 = nullptr;
      if (match(Y, m_LShr(m_Value(Smeared32), m_SpecificInt(1)))) {
        Value *Op0 = nullptr, *Op1 = nullptr;
        if (match(Smeared32,
                  m_c_Or(m_Value(Op0), m_LShr(m_Value(Op1), m_SpecificInt(32)))) &&
            Op0 == Op1)
          A = Op0;
      }
    }
    if (!A)
      return std::nullopt;

    Value *Peeled = peelSmear(A, {16, 8, 4, 2});
    Value *V = extractBaseV(Peeled);
    if (!V || !V->getType()->isIntegerTy(64))
      return std::nullopt;
    return DeBruijnMatch{LI, V, 64, 0};
  }

  if (Kind == DeBruijnKind::SEAL64) {
    constexpr uint64_t SEALMult = 0x07EDD5E59A4E28C2ULL;
    Value *Diff = nullptr;
    if (!match(M, m_c_Mul(m_Value(Diff), m_SpecificInt(SEALMult))))
      return std::nullopt;
    Value *Smeared = nullptr;
    Value *Op0 = nullptr, *Op1 = nullptr;
    if (match(Diff, m_Sub(m_Value(Op0), m_Value(Op1))) ||
        match(Diff, m_Xor(m_Value(Op0), m_Value(Op1)))) {
      Value *ShiftedBase = nullptr;
      if (match(Op1, m_LShr(m_Value(ShiftedBase), m_SpecificInt(1))) &&
          ShiftedBase == Op0) {
        Smeared = Op0;
      }
    }
    if (!Smeared)
      return std::nullopt;
    Value *Peeled = peelSmear(Smeared, {32, 16, 8, 4, 2});
    Value *V = extractBaseV(Peeled);
    if (!V || !V->getType()->isIntegerTy(64))
      return std::nullopt;
    return DeBruijnMatch{LI, V, 64, 63};
  }

  if (Kind == DeBruijnKind::Psnip64) {
    constexpr uint64_t PsnipMult = 0x03F79D71B4CB0A89ULL;
    Value *Smeared = nullptr;
    if (!match(M, m_Mul(m_Value(Smeared), m_SpecificInt(PsnipMult))))
      return std::nullopt;
    Value *Peeled = peelSmear(Smeared, {32, 16, 8, 4, 2});
    Value *V = extractBaseV(Peeled);
    if (!V || !V->getType()->isIntegerTy(64))
      return std::nullopt;
    return DeBruijnMatch{LI, V, 64, 0};
  }

  return std::nullopt;
}

static bool canonicalizeDeBruijnSequences(Function &F) {
  SmallVector<DeBruijnMatch, 4> Candidates;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (auto M = matchDeBruijn(LI))
          Candidates.push_back(*M);
      }
    }
  }
  if (Candidates.empty())
    return false;

  SmallVector<Instruction *, 8> DeadInsts;
  for (const auto &M : Candidates) {
    LoadInst *LI = M.LI;
    Value *V = M.V;
    IRBuilder<> Builder(LI);
    Value *Ctlz = Builder.CreateIntrinsic(Intrinsic::ctlz, {V->getType()},
                                          {V, Builder.getFalse()});
    Value *XorMask = Builder.CreateXor(
        Ctlz, ConstantInt::get(V->getType(), M.BitWidth - 1));
    Value *IsZero = Builder.CreateICmpEQ(V, ConstantInt::get(V->getType(), 0));
    Value *SafeVal = Builder.CreateSelect(
        IsZero, ConstantInt::get(V->getType(), M.ZeroVal), XorMask);
    Value *Replacement = Builder.CreateZExtOrTrunc(SafeVal, LI->getType());
    LI->replaceAllUsesWith(Replacement);
    DeadInsts.push_back(LI);
  }

  for (Instruction *Dead : DeadInsts)
    RecursivelyDeleteTriviallyDeadInstructions(Dead);

  return true;
}

} // namespace

PreservedAnalyses SAVIOIRPass::run(Function &F, FunctionAnalysisManager &FAM) {
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

  bool Changed = canonicalizeDeBruijnSequences(F);

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
      if (Arch.empty())
        return OptimizationRemarkMissed(DEBUG_TYPE, "NoPortInfo", &F)
               << "no port-info for architecture '"
               << Triple::getArchTypeName(
                      F.getParent()->getTargetTriple().getArch())
               << "'";
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
    case Bound::Memory:
      return "memory";
    }
    llvm_unreachable("unknown bound");
  };

  // Instructions without a port-info entry: TTI latency (1 if unknown) and one
  // micro-op unless TTI says the instruction is free (folded casts, phis,
  // debug intrinsics); no resource use (unless the port description sets
  // chargeOutOfGroup(): see outOfGroupOpCost in Partitioner.cpp).
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
      // The instances of a vector divide are lane groups; the remark counts
      // lanes.
      if (Reg.Groups[G].VectorDivide) {
        K *= Reg.Groups[G].GroupLanes;
        U *= Reg.Groups[G].GroupLanes;
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
    // The remarks above name operations the rewrite erases. The lane groups of
    // one vector divide are rewritten together in a single batch.
    struct VectorDivideBatch {
      Instruction *DivInst = nullptr;
      unsigned LaneGroupMask = 0;
      unsigned GroupLanes = 0;
    };
    SmallVector<VectorDivideBatch, 2> Divides;
    for (unsigned G : D.Promoted) {
      const Group &Grp = Reg.Groups[G];
      if (Grp.Dir == Direction::ToScalar) {
        rewriteScalarize(Grp, Reg.L);
        continue;
      }
      if (!Grp.VectorDivide) {
        rewriteSingletonVectorize(Grp, Reg.L);
        continue;
      }
      auto *It = find_if(Divides, [&](const VectorDivideBatch &B) {
        return B.DivInst == Grp.Ops[0];
      });
      if (It == Divides.end())
        It = Divides.insert(Divides.end(),
                            {Grp.Ops[0], 0u, Grp.GroupLanes});
      It->LaneGroupMask |= (1u << Grp.LaneGroup);
    }
    for (const auto &Batch : Divides)
      rewriteVectorDivide(Batch.DivInst, Batch.LaneGroupMask, Batch.GroupLanes);
    Changed = true;
  };

  bool Forced = SAVIOForce || SAVIOForceCount >= 0 || SAVIONoUnroll;
  unsigned LaneGroup = fpDivideGroup(*R.PD, TTI);
  for (Region &Reg : formRegions(F, LI, LaneGroup, R.PD.get())) {
    double RegionFrequency =
        double(BFI.getBlockFreq(Reg.BB).getFrequency()) / EntryFreq;
    if (RegionFrequency < SAVIOColdFraction ||
        (!Forced && !Reg.L && RegionFrequency < SAVIORemainderFraction))
      continue;
    // Unrolling is only for the cost-driven search; the force flags act on
    // the region as it is.
    ScalarEvolution *SE = nullptr;
    bool CanUnroll = false;
    if (!Forced && Reg.L) {
      SE = &FAM.getResult<ScalarEvolutionAnalysis>(F);
      CanUnroll = mayUnroll(*Reg.L, *SE);
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
        for (Region &New : formRegions(F, LI, LaneGroup, R.PD.get()))
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
