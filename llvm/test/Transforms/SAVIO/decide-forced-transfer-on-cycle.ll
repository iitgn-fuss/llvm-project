; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v-wide -savio-force -S %s | FileCheck %s
; acc = fshl(acc, 5) ^ v. The recurrence (fshl, xor) has an operation with no
; vector equivalent, so it is pinned scalar: even -savio-force cannot move the
; xor, since that would put GprToVec and VecToGpr transfers on the cycle (this
; test used to force exactly that, T 2.00 -> 4.00 in Part 1).
; CHECK-NOT: llvm.savio.sv
; CHECK: xor i32
; CHECK-NOT: llvm.savio.sv
target triple = "arm64-apple-macos"

declare i32 @llvm.fshl.i32(i32, i32, i32)

define i32 @f(ptr %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %v = load i32, ptr %pb
  %rot = call i32 @llvm.fshl.i32(i32 %acc, i32 %acc, i32 5)
  %acc.next = xor i32 %rot, %v
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.next
}
