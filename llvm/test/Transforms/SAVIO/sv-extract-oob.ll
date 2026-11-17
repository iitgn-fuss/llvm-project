; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force. A constant out-of-range lane is poison; it must not
; crash the shuffle builder and enters as a scalar insert.
target triple = "arm64-apple-macos"

define void @f(<4 x i32> %v, ptr %p, ptr %r) {
; CHECK-LABEL: @f(
; CHECK-NOT:     shufflevector
; CHECK:         call <4 x i32> @llvm.savio.sv.add.v4i32(
  %e = extractelement <4 x i32> %v, i32 9
  %x = load i32, ptr %p
  %s = add i32 %e, %x
  store i32 %s, ptr %r
  ret void
}
