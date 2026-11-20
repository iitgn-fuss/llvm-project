; RUN: opt -passes=savio-ir -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | opt -passes=verify -disable-output
; Tests singleton vectorization of llvm.fshl and llvm.fshr intrinsics.
target triple = "x86_64-unknown-linux-gnu"

define void @test_fshl(ptr %p, ptr %q, ptr %r) {
; CHECK-LABEL: @test_fshl(
; CHECK:         [[A:%.*]] = load i32, ptr %p
; CHECK:         [[B:%.*]] = load i32, ptr %q
; CHECK:         [[VA:%.*]] = insertelement <4 x i32> poison, i32 [[A]], i32 0
; CHECK-NEXT:    [[VB:%.*]] = insertelement <4 x i32> poison, i32 [[B]], i32 0
; CHECK-NEXT:    [[ROT:%.*]] = call <4 x i32> @llvm.fshl.v4i32(<4 x i32> [[VA]], <4 x i32> [[VB]], <4 x i32> splat (i32 5))
; CHECK-NEXT:    [[E:%.*]] = extractelement <4 x i32> [[ROT]], i32 0
; CHECK-NEXT:    store i32 [[E]], ptr %r
; CHECK:         ret void
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %rot = call i32 @llvm.fshl.i32(i32 %a, i32 %b, i32 5)
  store i32 %rot, ptr %r
  ret void
}

define void @test_fshr(ptr %p, ptr %q, ptr %r) {
; CHECK-LABEL: @test_fshr(
; CHECK:         [[A:%.*]] = load i32, ptr %p
; CHECK:         [[B:%.*]] = load i32, ptr %q
; CHECK:         [[VA:%.*]] = insertelement <4 x i32> poison, i32 [[A]], i32 0
; CHECK-NEXT:    [[VB:%.*]] = insertelement <4 x i32> poison, i32 [[B]], i32 0
; CHECK-NEXT:    [[ROT:%.*]] = call <4 x i32> @llvm.fshr.v4i32(<4 x i32> [[VA]], <4 x i32> [[VB]], <4 x i32> splat (i32 7))
; CHECK-NEXT:    [[E:%.*]] = extractelement <4 x i32> [[ROT]], i32 0
; CHECK-NEXT:    store i32 [[E]], ptr %r
; CHECK:         ret void
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %rot = call i32 @llvm.fshr.i32(i32 %a, i32 %b, i32 7)
  store i32 %rot, ptr %r
  ret void
}

declare i32 @llvm.fshl.i32(i32, i32, i32)
declare i32 @llvm.fshr.i32(i32, i32, i32)
