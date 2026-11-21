; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output

; When udiv and urem share operands in the same basic block and one reaches an address
; (requiring scalar execution), the sibling is kept scalar to preserve hardware div/rem fusion.
target triple = "x86_64-unknown-linux-gnu"

define i32 @test_paired_divrem(i32 %x, i32 %y, ptr %base) {
; CHECK-LABEL: @test_paired_divrem(
; CHECK-NOT:     llvm.savio.sv
; CHECK:         %q = udiv i32 %x, %y
; CHECK:         %r = urem i32 %x, %y
  %q = udiv i32 %x, %y
  %r = urem i32 %x, %y
  %idx = zext i32 %q to i64
  %ptr = getelementptr inbounds i32, ptr %base, i64 %idx
  %val = load i32, ptr %ptr
  %sum = add i32 %r, %val
  ret i32 %sum
}
