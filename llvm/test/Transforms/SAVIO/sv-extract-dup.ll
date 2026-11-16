; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force. The same extractelement feeds the group three times:
; one shuffle, and the extract is erased once (regression: double erase).
target triple = "arm64-apple-macos"

define void @f(<4 x i32> %v, ptr %p, ptr %r) {
; CHECK-LABEL: @f(
; CHECK:         [[L:%.*]] = shufflevector <4 x i32> %v, <4 x i32> poison, <4 x i32> <i32 1, i32 poison, i32 poison, i32 poison>
; CHECK-NOT:     shufflevector
; CHECK:         call <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32> [[L]], <4 x i32> [[L]])
; CHECK:         call <4 x i32> @llvm.savio.sv.mul.v4i32(<4 x i32> {{%.*}}, <4 x i32> [[L]])
; CHECK-NOT:     extractelement <4 x i32> %v
  %e = extractelement <4 x i32> %v, i32 1
  %x = load i32, ptr %p
  %s = add i32 %e, %e
  %t = mul i32 %s, %e
  %u = xor i32 %t, %x
  store i32 %u, ptr %r
  ret void
}
