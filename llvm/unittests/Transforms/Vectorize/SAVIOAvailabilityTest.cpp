//===- SAVIOAvailabilityTest.cpp - SAVIO availability table tests ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SAVIO/Availability.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Path.h"
#include "llvm/Transforms/Vectorize/SAVIO/PortDescription.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::savio;

namespace {

std::string inputsRoot() {
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "port-info");
  return std::string(Root);
}

using Keys = SmallVector<std::string, 8>;

TEST(SAVIOAvailability, RequiredKeysSingletonVectorize) {
  LLVMContext Ctx;
  Type *I32 = Type::getInt32Ty(Ctx);
  EXPECT_EQ(requiredKeys(Transform::SingletonVectorize, "add", I32),
            (Keys{"sv.add v4i32", "gpr_to_vec", "vec_to_gpr"}));
  EXPECT_EQ(requiredKeys(Transform::SingletonVectorize, "shl.imm", I32),
            (Keys{"sv.shl.imm v4i32", "gpr_to_vec", "vec_to_gpr"}));
}

TEST(SAVIOAvailability, SingletonVectorizeCtpopKeys) {
  LLVMContext Ctx;
  Type *I32 = Type::getInt32Ty(Ctx);
  EXPECT_EQ(requiredKeys(Transform::SingletonVectorize, "ctpop", I32, 0),
            (Keys{"sv.ctpop v4i32", "gpr_to_vec", "vec_to_gpr"}));
  EXPECT_EQ(requiredKeys(Transform::SingletonVectorize, "ctlz", I32, 0),
            (Keys{"sv.ctlz v4i32", "gpr_to_vec", "vec_to_gpr"}));
}

TEST(SAVIOAvailability, RequiredKeysFpDivideLanes) {
  LLVMContext Ctx;
  Type *V8 = FixedVectorType::get(Type::getInt32Ty(Ctx), 8);
  EXPECT_EQ(requiredKeys(Transform::FpDivideLanes, "udiv", V8, 4),
            (Keys{"udiv v8i32", "fpdiv4.udiv v8i32"}));
  EXPECT_EQ(requiredKeys(Transform::FpDivideLanes, "urem", V8, 8),
            (Keys{"urem v8i32", "fpdiv8.512.urem v8i32"}));
}

TEST(SAVIOAvailability, FpDivideKeyNamesTheEmittedWidth) {
  LLVMContext Ctx;
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *V8 = FixedVectorType::get(I32, 8), *V16 = FixedVectorType::get(I32, 16);
  // G = 4: fpdiv8 on a v8i32 is two ymm sequences, the plain key.
  EXPECT_EQ(fpDivideKey("udiv", V8, 8, 4), "fpdiv8.udiv v8i32");
  // G = 8: one zmm divide, its own key.
  EXPECT_EQ(fpDivideKey("udiv", V8, 8, 8), "fpdiv8.512.udiv v8i32");
  // A 512-bit vector is zmm code at any G: the plain key.
  EXPECT_EQ(fpDivideKey("urem", V16, 8, 4), "fpdiv8.urem v16i32");
  EXPECT_EQ(fpDivideKey("urem", V16, 8, 8), "fpdiv8.urem v16i32");
  EXPECT_EQ(fpDivideKey("urem", V16, 16, 8), "fpdiv16.urem v16i32");
}

TEST(SAVIOAvailability, RequiredKeysScalarize) {
  LLVMContext Ctx;
  Type *V4 = FixedVectorType::get(Type::getInt64Ty(Ctx), 4);
  EXPECT_EQ(requiredKeys(Transform::Scalarize, "mul", V4),
            (Keys{"mul v4i64", "mul i64", "load v4i64", "load i64",
                  "store v4i64", "store i64", "lane_extract", "lane_insert"}));
}

TEST(SAVIOAvailability, FirstMissing) {
  auto PD = PortDescription::load(inputsRoot(), "AArch64", "test2s1v");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  LLVMContext Ctx;
  Type *I32 = Type::getInt32Ty(Ctx);
  // Everything Singleton Vectorize of add needs is there.
  EXPECT_FALSE(firstMissing(
      **PD, requiredKeys(Transform::SingletonVectorize, "add", I32)));
  // A probe key without a cost, a transfer without a cost: the first of the
  // keys that is missing is reported.
  EXPECT_EQ(firstMissing(**PD, Keys{"add i32", "nosuch v4i32", "lane_extract"}),
            "nosuch v4i32");
  EXPECT_EQ(firstMissing(**PD, Keys{"add i32", "lane_extract", "lane_insert"}),
            "lane_extract");
  EXPECT_FALSE(firstMissing(**PD, Keys{}));
}

TEST(SAVIOAvailability, ScalarizeNeedsPerWidthOrUntypedLaneKeys) {
  SmallString<128> Root(SAVIO_TEST_INPUTS);
  sys::path::append(Root, "..", "X86", "Inputs", "port-info");
  // Only per-width keys (v8i32, v16i32): they stand in for the untyped ones,
  // for those widths only.
  auto PD = PortDescription::load(Root, "x86", "test-avx512-lane-widths-only");
  ASSERT_TRUE(!!PD) << toString(PD.takeError());
  Keys Lane{"lane_extract", "lane_insert"};
  EXPECT_FALSE((*PD)->transferCost(TransferKind::LaneExtract).has_value());
  EXPECT_EQ(firstMissing(**PD, Lane), "lane_extract");
  EXPECT_FALSE(firstMissing(**PD, Lane, "v8i32"));
  EXPECT_EQ(firstMissing(**PD, Lane, "v8i64"), "lane_extract");
  // The untyped keys alone still satisfy any width.
  auto Both = PortDescription::load(Root, "x86", "test-avx512-lane-widths");
  ASSERT_TRUE(!!Both) << toString(Both.takeError());
  EXPECT_FALSE(firstMissing(**Both, Lane, "v8i64"));
}

TEST(SAVIOAvailability, TransformNames) {
  EXPECT_EQ(transformName(Transform::SingletonVectorize), "singleton-vectorize");
  EXPECT_EQ(transformName(Transform::FpDivideLanes), "fpdiv-lanes");
  EXPECT_EQ(transformName(Transform::Scalarize), "scalarize");
}

} // namespace
