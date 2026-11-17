; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; An fmix-style loop-carried chain of only in-scope operations (xor, lshr, mul)
; through one phi: every operation of the recurrence has a vector equivalent,
; so the whole recurrence moves, with a <4 x i32> phi.
; CHECK: phi <4 x i32>
; CHECK: call <4 x i32> @llvm.savio.sv.lshr
; CHECK: call <4 x i32> @llvm.savio.sv.xor
; CHECK: call <4 x i32> @llvm.savio.sv.mul
target triple = "arm64-apple-macos"

define i32 @f(ptr %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %h = phi i32 [ 1, %entry ], [ %h.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %v = load i32, ptr %pb
  %x = xor i32 %h, %v
  %s = lshr i32 %x, 16
  %y = xor i32 %x, %s
  %h.next = mul i32 %y, -2048144789
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %h.next
}
