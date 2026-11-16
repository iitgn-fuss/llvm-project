//===- SAVIOPortDescriptionTest.cpp - SAVIO port description tests -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::savio;

namespace {

std::string inputsRoot() {
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  return std::string(Root);
}

std::optional<unsigned> classId(const PortDescription &PD, StringRef Name) {
  for (unsigned C = 0; C != PD.numClasses(); ++C)
    if (PD.className(C) == Name)
      return C;
  return std::nullopt;
}

/// Copies the test2s1v fixture to <tmp>/AArch64/test2s1v and returns <tmp>.
std::string copyFixture() {
  SmallString<128> Tmp;
  EXPECT_FALSE(sys::fs::createUniqueDirectory("savio-pd", Tmp));
  SmallString<128> Dst(Tmp);
  sys::path::append(Dst, "AArch64", "test2s1v");
  EXPECT_FALSE(sys::fs::create_directories(Dst));
  for (StringRef F :
       {"resources.json", "ops.json", "transfers.json", "probes.json"}) {
    SmallString<128> Src(inputsRoot());
    sys::path::append(Src, "AArch64", "test2s1v", F);
    SmallString<128> To(Dst);
    sys::path::append(To, F);
    EXPECT_FALSE(sys::fs::copy_file(Src, To));
  }
  return std::string(Tmp);
}

void rewrite(StringRef Root, StringRef File, StringRef From, StringRef To) {
  SmallString<128> P(Root);
  sys::path::append(P, "AArch64", "test2s1v", File);
  auto Buf = MemoryBuffer::getFile(P);
  ASSERT_TRUE(!!Buf);
  std::string S = (*Buf)->getBuffer().str();
  size_t Pos = S.find(From);
  ASSERT_NE(Pos, std::string::npos);
  S.replace(Pos, From.size(), To);
  std::error_code EC;
  raw_fd_ostream OS(P, EC);
  ASSERT_FALSE(EC);
  OS << S;
}

std::string loadError(StringRef Root) {
  auto PD = PortDescription::load(Root, "AArch64", "test2s1v");
  EXPECT_FALSE(!!PD);
  return PD ? "" : toString(PD.takeError());
}

TEST(SAVIOPortDescription, LoadsFixture) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  auto Scalar = classId(**PD, "scalar"), Vector = classId(**PD, "vector");
  ASSERT_TRUE(Scalar && Vector);
  EXPECT_EQ((*PD)->units(*Scalar), 2u);
  EXPECT_EQ((*PD)->units(*Vector), 1u);
  EXPECT_EQ((*PD)->dispatchWidth(), 4u);
  EXPECT_EQ((*PD)->numClasses(), 4u);
}

TEST(SAVIOPortDescription, ProbeSumsOpcodes) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  unsigned Div = *classId(**PD, "int_div"), Scalar = *classId(**PD, "scalar");
  std::optional<OpCost> C = (*PD)->irOpCost("urem i32");
  ASSERT_TRUE(C.has_value());
  ASSERT_EQ(C->Uses.size(), 2u);
  for (const ResourceUse &U : C->Uses) {
    if (U.Class == Div)
      EXPECT_EQ(U.Cycles, 4.0);
    else {
      EXPECT_EQ(U.Class, Scalar);
      EXPECT_EQ(U.Cycles, 1.0);
    }
  }
  EXPECT_EQ(C->Latency, 10u);

  std::optional<OpCost> V = (*PD)->irOpCost("sv.udiv v4i32");
  ASSERT_TRUE(V.has_value());
  EXPECT_EQ(V->Latency, 14u);
  EXPECT_TRUE((*PD)->irOpCost("sv.shl.imm v4i32").has_value());
  EXPECT_FALSE((*PD)->irOpCost("add i64").has_value());
}

TEST(SAVIOPortDescription, Transfers) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  std::optional<OpCost> G = (*PD)->transferCost(TransferKind::GprToVec);
  ASSERT_TRUE(G.has_value());
  EXPECT_EQ(G->Latency, 1u);
  ASSERT_EQ(G->Uses.size(), 1u);
  EXPECT_EQ(G->Uses[0].Class, *classId(**PD, "vector"));
  std::optional<OpCost> L = (*PD)->transferCost(TransferKind::LoadToVec);
  ASSERT_TRUE(L.has_value());
  EXPECT_TRUE(L->Uses.empty());
  EXPECT_EQ(L->Latency, 0u);
}

TEST(SAVIOPortDescription, MissingCPUIsNotFound) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "nope");
  ASSERT_FALSE(!!PD);
  std::error_code EC = errorToErrorCode(PD.takeError());
  EXPECT_EQ(EC, std::make_error_code(std::errc::no_such_file_or_directory));
}

TEST(SAVIOPortDescription, UnknownClassIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"class\": \"scalar\"", "\"class\": \"bogus\"");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("ops.json"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("bogus"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, MissingSourceIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"source\"", "\"sauce\"");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("ops.json"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("source"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, UnknownProbeOpcodeIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "probes.json", "\"T_ADD\"", "\"T_NOPE\"");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("probes.json"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("T_NOPE"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

const char *SeqJson =
    "\"sequences\": [{\"key\": \"sv.urem v4i32\", \"latency\": 20, "
    "\"uops\": 7, \"uses\": [{\"class\": \"vector\", \"cycles\": 12}], "
    "\"source\": \"measured\", \"evidence\": \"unit test\"}],\n  \"ops\": [";

TEST(SAVIOPortDescription, SequenceOverridesSum) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"ops\": [", SeqJson);
  auto PD = PortDescription::load(Root, "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  std::optional<OpCost> C = (*PD)->irOpCost("sv.urem v4i32");
  ASSERT_TRUE(C.has_value());
  EXPECT_EQ(C->Latency, 20u);
  EXPECT_EQ(C->Uops, 7u);
  ASSERT_EQ(C->Uses.size(), 1u);
  EXPECT_EQ(C->Uses[0].Class, *classId(**PD, "vector"));
  EXPECT_EQ(C->Uses[0].Cycles, 12.0);
  // Other keys still sum their opcodes.
  EXPECT_EQ((*PD)->irOpCost("urem i32")->Latency, 10u);
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, SequenceAbsentFallsBack) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  std::optional<OpCost> C = (*PD)->irOpCost("sv.urem v4i32");
  ASSERT_TRUE(C.has_value());
  // Part 1 format: the sum over the probe's opcodes (T_VUDIV_FP + T_VUDIV_FIX).
  EXPECT_EQ(C->Latency, 14u);
  EXPECT_EQ(C->Uops, 2u);
  ASSERT_EQ(C->Uses.size(), 2u);
  for (const ResourceUse &U : C->Uses) {
    if (U.Class == *classId(**PD, "fp_div"))
      EXPECT_EQ(U.Cycles, 4.0);
    else {
      EXPECT_EQ(U.Class, *classId(**PD, "vector"));
      EXPECT_EQ(U.Cycles, 3.0);
    }
  }
}

TEST(SAVIOPortDescription, SequenceUnknownKeyIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"ops\": [",
          "\"sequences\": [{\"key\": \"nope v4i32\", \"latency\": 1, "
          "\"uops\": 1, \"uses\": [], \"source\": \"measured\", "
          "\"evidence\": \"x\"}],\n  \"ops\": [");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("nope v4i32"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, SequenceMissingEvidenceIsError) {
  std::string Root = copyFixture();
  rewrite(Root, "ops.json", "\"ops\": [", SeqJson);
  rewrite(Root, "ops.json", "\"evidence\": \"unit test\"", "\"x\": 1");
  std::string Msg = loadError(Root);
  EXPECT_NE(Msg.find("evidence"), std::string::npos) << Msg;
  sys::fs::remove_directories(Root);
}

TEST(SAVIOPortDescription, ArchDir) {
  EXPECT_EQ(archDirFor(Triple("arm64-apple-macos")), "AArch64");
  EXPECT_EQ(archDirFor(Triple("aarch64-linux-gnu")), "AArch64");
  EXPECT_EQ(archDirFor(Triple("x86_64-linux-gnu")), "");
}

} // namespace
