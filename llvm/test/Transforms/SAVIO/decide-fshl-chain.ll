; RUN: opt -passes=savio-ir -savio-port-info=%S/../../../../../src/port-info -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; A hash accumulator whose recurrence runs through llvm.fshl, an operation SAVIO
; has no port-info for: acc = fshl(acc + b[i]*P2, 13) * P1. The fshl latency
; still counts, so the loop is bound by its recurrence, and no group is moved.
; CHECK: remark: {{.*}}T {{[0-9.]+}} (recurrence)
; CHECK-NOT: llvm.savio.sv
target triple = "arm64-apple-macosx15.0.0"

declare i32 @llvm.fshl.i32(i32, i32, i32)

define i32 @add_gpr(ptr %b, i64 %n) #0 {
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
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret i32 %acc.next
}
attributes #0 = { "target-cpu"="apple-m4" }
