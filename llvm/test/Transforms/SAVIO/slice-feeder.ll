; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; add_gpr: acc = fshl(acc + b[i]*P2, 13) * P1. The recurrence (add, fshl, mul)
; contains fshl, which has no vector equivalent, so it is pinned scalar whole.
; Only the feeder b[i]*P2, a slice off the cycle, may move.
; CHECK-COUNT-1: call <4 x i32> @llvm.savio.sv.mul
; CHECK-NOT: call {{.*}}@llvm.savio.sv
; CHECK: add i32
; CHECK: mul i32
; CHECK-NOT: call {{.*}}@llvm.savio.sv
target triple = "arm64-apple-macos"

declare i32 @llvm.fshl.i32(i32, i32, i32)

define i32 @add_gpr(ptr %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %v = load i32, ptr %pb
  %m = mul i32 %v, -1640531535
  %s = add i32 %acc, %m
  %r = call i32 @llvm.fshl.i32(i32 %s, i32 %s, i32 13)
  %acc.next = mul i32 %r, 1597334677
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.next
}
