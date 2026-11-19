; RUN: opt -passes=savio-ir -savio-force -savio-relax-poison-flags -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-relax-poison-flags -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output

; When -savio-relax-poison-flags is set, operations with poison flags (nuw, nsw, exact)
; are safely vectorized via standard refinement semantics.
target triple = "arm64-apple-macos"

define void @f(ptr %p, ptr %q, ptr %r) {
; CHECK-LABEL: @f(
; CHECK: insertelement <4 x i32>
; CHECK: call <4 x i32> @llvm.savio.sv.add.v4i32(
; CHECK: call <4 x i32> @llvm.savio.sv.sub.v4i32(
; CHECK: call <4 x i32> @llvm.savio.sv.mul.v4i32(
; CHECK: call <4 x i32> @llvm.savio.sv.lshr.v4i32(
; CHECK: extractelement <4 x i32>
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %n = add nuw i32 %a, 1
  %s = sub nsw i32 %n, %b
  %m = mul nuw nsw i32 %s, 3
  %h = lshr exact i32 %m, 2
  store i32 %h, ptr %r
  ret void
}
