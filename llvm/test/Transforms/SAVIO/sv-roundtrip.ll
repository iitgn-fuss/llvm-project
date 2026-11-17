; RUN: opt -passes='savio-ir,instcombine' -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes='savio-ir,instcombine' -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force. InstCombine must not fold the opaque intrinsics back
; into scalar or plain vector operations.
target triple = "arm64-apple-macos"

define void @f(ptr %p, ptr %q, ptr %r) {
; CHECK-LABEL: @f(
; CHECK:         call <4 x i32> @llvm.savio.sv.mul.v4i32(
; CHECK:         call <4 x i32> @llvm.savio.sv.xor.v4i32(
; CHECK:         call <4 x i32> @llvm.savio.sv.lshr.v4i32(
; CHECK:         extractelement <4 x i32> {{%.*}}, i{{32|64}} 0
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %m = mul i32 %a, %b
  %x = xor i32 %m, %a
  %s = lshr i32 %x, 3
  store i32 %s, ptr %r
  ret void
}
