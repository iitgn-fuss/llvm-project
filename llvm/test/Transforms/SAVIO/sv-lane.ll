; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force: the add group is promoted. An operand that is an
; extractelement enters lane 0 with a shufflevector, not a GPR round trip.
target triple = "arm64-apple-macos"

define void @f(<4 x i32> %v, ptr %p, ptr %r) {
; CHECK-LABEL: @f(
; CHECK:         [[L:%.*]] = shufflevector <4 x i32> %v, <4 x i32> poison, <4 x i32> <i32 2, i32 poison, i32 poison, i32 poison>
; CHECK:         call <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32> [[L]], <4 x i32> {{%.*}})
; CHECK-NOT:     extractelement <4 x i32> %v
  %e = extractelement <4 x i32> %v, i32 2
  %x = load i32, ptr %p
  %s = add i32 %e, %x
  store i32 %s, ptr %r
  ret void
}
