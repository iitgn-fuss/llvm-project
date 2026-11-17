; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force. A variable lane index cannot be shuffled into lane 0,
; so the extract stays and its scalar result enters with an insertelement.
target triple = "arm64-apple-macos"

define void @f(<4 x i32> %v, i32 %i, ptr %p, ptr %r) {
; CHECK-LABEL: @f(
; CHECK:         [[E:%.*]] = extractelement <4 x i32> %v, i32 %i
; CHECK-NOT:     shufflevector
; CHECK:         insertelement <4 x i32> poison, i32 [[E]], i32 0
; CHECK:         call <4 x i32> @llvm.savio.sv.add.v4i32(
  %e = extractelement <4 x i32> %v, i32 %i
  %x = load i32, ptr %p
  %s = add i32 %e, %x
  store i32 %s, ptr %r
  ret void
}
