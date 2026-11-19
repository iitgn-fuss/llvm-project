//===- SAVIOTestUtils.h - Shared SAVIO unit-test fixtures ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_UNITTESTS_TRANSFORMS_VECTORIZE_SAVIOTESTUTILS_H
#define LLVM_UNITTESTS_TRANSFORMS_VECTORIZE_SAVIOTESTUTILS_H

#include "llvm/Transforms/Vectorize/SAVIO/Partitioner.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <memory>
#include <string>

using namespace llvm;
using namespace llvm::savio;

namespace {

class SAVIOClassifyInputTest : public testing::Test {
protected:
  LLVMContext Ctx;
  std::unique_ptr<Module> M;

  Function *parse(StringRef IR) {
    SMDiagnostic Err;
    M = parseAssemblyString(IR, Err, Ctx);
    if (!M) {
      std::string Msg;
      raw_string_ostream OS(Msg);
      Err.print("SAVIOClassifyInputTest", OS);
      ADD_FAILURE() << Msg;
      return nullptr;
    }
    return M->getFunction("f");
  }

  static Instruction *find(Function &F, StringRef Name) {
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (I.getName() == Name)
          return &I;
    return nullptr;
  }

  static Group groupOf(std::initializer_list<Instruction *> Ops) {
    Group G;
    for (Instruction *I : Ops)
      G.Ops.push_back(I);
    return G;
  }
};

struct SearchCoreCounter {
  unsigned Calls;
};

extern "C" inline double searchCoreTestEstimate(void *, const unsigned *Moved,
                                                unsigned NumClasses) {
  if (NumClasses != 2)
    return 100.0;
  double D0 = double(Moved[0]) - 2.0;
  double D1 = double(Moved[1]) - 1.0;
  return D0 * D0 + D1 * D1;
}

extern "C" inline double searchCoreCounterEstimate(void *Raw,
                                                   const unsigned *Moved,
                                                   unsigned) {
  auto *Counter = static_cast<SearchCoreCounter *>(Raw);
  ++Counter->Calls;
  return Moved[0] == 1 ? 1.0 : 2.0;
}

static const char *LoopIR = R"(
  define void @f(i32 %a, <4 x i32> %vec, i32 %idx, ptr %p, ptr %q, i64 %n) {
  entry:
    %inv = add i32 %a, 1
    br label %loop
  loop:
    %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
    %ld = load i32, ptr %p
    %ld2 = load i32, ptr %q
    %e2 = extractelement <4 x i32> %vec, i32 2
    %evar = extractelement <4 x i32> %vec, i32 %idx
    %eoob = extractelement <4 x i32> %vec, i32 7
    %g = add i32 %ld, %a
    %g2 = add i32 %ld2, %ld2
    %g3 = add i32 %inv, %e2
    %g4 = add i32 %evar, %eoob
    %i.next = add i64 %i, 1
    %c = icmp ult i64 %i.next, %n
    br i1 %c, label %loop, label %exit
  exit:
    ret void
  })";

inline Decision decideForcedOn(Function *F,
                               std::unique_ptr<PortDescription> &PD,
                               Region &Out,
                               SmallVector<Region, 4> &Regions,
                               LoopInfo &LI) {
  Regions = formRegions(*F, LI);
  EXPECT_EQ(Regions.size(), 1u);
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  auto PDOrErr = PortDescription::load(Root, "AArch64", "test3scv");
  EXPECT_TRUE(!!PDOrErr);
  PD = std::move(*PDOrErr);
  auto Fallback = [](const Instruction &) {
    OpCost C;
    C.Latency = 1;
    C.Uops = 1;
    return C;
  };
  bool Saved = SAVIOForce;
  SAVIOForce = true;
  Decision D = decide(Regions[0], *PD, Fallback, 0.05);
  SAVIOForce = Saved;
  return D;
}

inline std::string bitCountLoop(const char *Fn) {
  std::string IR = std::string("declare i32 @llvm.") + Fn + ".i32(i32" +
                   (StringRef(Fn) == "ctlz" ? ", i1 immarg" : "") + ")\n";
  IR += "define void @f(ptr %b, ptr %o, i64 %n) {\nentry:\n  br label %loop\n"
        "loop:\n  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]\n";
  for (int K = 0; K != 4; ++K) {
    std::string N = std::to_string(K);
    IR += "  %pb" + N + " = getelementptr inbounds i32, ptr %b, i64 " + N +
          "\n";
    IR += "  %b" + N + " = load i32, ptr %pb" + N + "\n";
    IR += "  %r" + N + " = call i32 @llvm." + Fn + ".i32(i32 %b" + N +
          (StringRef(Fn) == "ctlz" ? ", i1 false" : "") + ")\n";
    IR += "  %po" + N + " = getelementptr inbounds i32, ptr %o, i64 " + N +
          "\n";
    IR += "  store i32 %r" + N + ", ptr %po" + N + "\n";
  }
  IR += "  %i.next = add nuw i64 %i, 1\n  %cmp = icmp ult i64 %i.next, %n\n"
        "  br i1 %cmp, label %loop, label %exit\nexit:\n  ret void\n}\n";
  return IR;
}

inline Expected<std::unique_ptr<PortDescription>>
loadX86Fixture(StringRef CPU) {
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "..", "X86", "Inputs", "port-info");
  return PortDescription::load(Root, "x86", CPU);
}

} // namespace

#endif // LLVM_UNITTESTS_TRANSFORMS_VECTORIZE_SAVIOTESTUTILS_H
