; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force: the single group (mul, xor, lshr) is Singleton
; Vectorized. The chain stays in vector form: no extractelement between the
; calls, one insertelement per load, one extractelement at the store.
target triple = "arm64-apple-macos"

define void @f(ptr %p, ptr %q, ptr %r) {
; CHECK-LABEL: @f(
; CHECK:         [[A:%.*]] = load i32, ptr %p
; CHECK:         [[B:%.*]] = load i32, ptr %q
; CHECK-NOT:     extractelement
; CHECK:         [[VA:%.*]] = insertelement <4 x i32> poison, i32 [[A]], i32 0
; CHECK-NEXT:    [[VB:%.*]] = insertelement <4 x i32> poison, i32 [[B]], i32 0
; CHECK-NEXT:    [[M:%.*]] = call <4 x i32> @llvm.savio.sv.mul.v4i32(<4 x i32> [[VA]], <4 x i32> [[VB]])
; CHECK-NOT:     extractelement
; CHECK-NEXT:    [[X:%.*]] = call <4 x i32> @llvm.savio.sv.xor.v4i32(<4 x i32> [[M]], <4 x i32> [[VA]])
; CHECK-NOT:     extractelement
; CHECK-NEXT:    [[S:%.*]] = call <4 x i32> @llvm.savio.sv.lshr.v4i32(<4 x i32> [[X]], <4 x i32> splat (i32 3))
; CHECK-NEXT:    [[E:%.*]] = extractelement <4 x i32> [[S]], i32 0
; CHECK-NEXT:    store i32 [[E]], ptr %r
; CHECK-NOT:     extractelement
; CHECK-NOT:     mul i32
; CHECK:         ret void
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %m = mul i32 %a, %b
  %x = xor i32 %m, %a
  %s = lshr i32 %x, 3
  store i32 %s, ptr %r
  ret void
}
