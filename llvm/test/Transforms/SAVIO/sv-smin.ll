; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Tests singleton vectorization of llvm.smin and llvm.smax intrinsics.
target triple = "arm64-apple-macos"

define void @test_smin_smax(ptr %p, ptr %q, ptr %r) {
; CHECK-LABEL: @test_smin_smax(
; CHECK:         [[A:%.*]] = load i32, ptr %p
; CHECK:         [[B:%.*]] = load i32, ptr %q
; CHECK:         [[VA:%.*]] = insertelement <4 x i32> poison, i32 [[A]], i32 0
; CHECK-NEXT:    [[VB:%.*]] = insertelement <4 x i32> poison, i32 [[B]], i32 0
; CHECK-NEXT:    [[MIN:%.*]] = call <4 x i32> @llvm.smin.v4i32(<4 x i32> [[VA]], <4 x i32> [[VB]])
; CHECK-NEXT:    [[MAX:%.*]] = call <4 x i32> @llvm.smax.v4i32(<4 x i32> [[MIN]], <4 x i32> [[VA]])
; CHECK-NEXT:    [[E:%.*]] = extractelement <4 x i32> [[MAX]], i32 0
; CHECK-NEXT:    store i32 [[E]], ptr %r
; CHECK:         ret void
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %min = call i32 @llvm.smin.i32(i32 %a, i32 %b)
  %max = call i32 @llvm.smax.i32(i32 %min, i32 %a)
  store i32 %max, ptr %r
  ret void
}

declare i32 @llvm.smin.i32(i32, i32)
declare i32 @llvm.smax.i32(i32, i32)
