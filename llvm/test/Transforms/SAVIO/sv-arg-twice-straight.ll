; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; A function argument used twice by one group is moved to lane 0 once and
; reused (it is cached as an input, never mistaken for a group member).
target triple = "arm64-apple-macos"

define i32 @f(i32 %a, i32 %b, i32 %p) {
; CHECK-LABEL: @f(
; CHECK:         [[VP:%.*]] = insertelement <4 x i32> poison, i32 %p, i32 0
; CHECK-COUNT-2: call <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32> {{%.*}}, <4 x i32> [[VP]])
  %x = urem i32 %a, %p
  %y = urem i32 %b, %p
  %s = add i32 %x, %y
  ret i32 %s
}
