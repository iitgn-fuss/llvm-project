; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force: the udiv and the urem group are both promoted.
; A zero divisor in lane 0 is poison, not UB, so the intrinsic is speculatable.
target triple = "arm64-apple-macos"

define void @f(ptr %p, ptr %q, ptr %r, ptr %t) {
; CHECK-LABEL: @f(
; CHECK:         call <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32> {{%.*}}, <4 x i32> {{%.*}})
; CHECK:         call <4 x i32> @llvm.savio.sv.udiv.v4i32(<4 x i32> {{%.*}}, <4 x i32> {{%.*}})
; CHECK-NOT:     urem i32
; CHECK-NOT:     udiv i32
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %m = urem i32 %a, %b
  store i32 %m, ptr %r
  %c = load i32, ptr %r
  %d = udiv i32 %c, %b
  store i32 %d, ptr %t
  ret void
}
