; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Tests singleton vectorization of llvm.cttz intrinsic.
target triple = "arm64-apple-macos"

define void @test_cttz(ptr %p, ptr %r) {
; CHECK-LABEL: @test_cttz(
; CHECK:         [[A:%.*]] = load i32, ptr %p
; CHECK:         [[VA:%.*]] = insertelement <4 x i32> poison, i32 [[A]], i32 0
; CHECK-NEXT:    [[CTZ:%.*]] = call <4 x i32> @llvm.savio.sv.cttz.v4i32(<4 x i32> [[VA]])
; CHECK-NEXT:    [[E:%.*]] = extractelement <4 x i32> [[CTZ]], i32 0
; CHECK-NEXT:    store i32 [[E]], ptr %r
; CHECK:         ret void
  %a = load i32, ptr %p
  %ctz = call i32 @llvm.cttz.i32(i32 %a, i1 false)
  store i32 %ctz, ptr %r
  ret void
}

declare i32 @llvm.cttz.i32(i32, i1)
