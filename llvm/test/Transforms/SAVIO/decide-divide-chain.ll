; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks=savio -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s
; Chained urems carried by a phi: the recurrence limits T, so nothing is promoted.
; CHECK: remark: {{.*}}T 40.00 (recurrence)
; CHECK-NOT: remark:
target triple = "arm64-apple-macos"

define void @f(ptr %a, ptr %c) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 12345, %entry ], [ %r3, %loop ]
  %pa0 = getelementptr i32, ptr %a, i64 %i
  %x0 = load i32, ptr %pa0
  %r0 = urem i32 %acc, %x0
  %pa1 = getelementptr i32, ptr %a, i64 %i
  %x1 = load i32, ptr %pa1
  %r1 = urem i32 %r0, %x1
  %pa2 = getelementptr i32, ptr %a, i64 %i
  %x2 = load i32, ptr %pa2
  %r2 = urem i32 %r1, %x2
  %pa3 = getelementptr i32, ptr %a, i64 %i
  %x3 = load i32, ptr %pa3
  %r3 = urem i32 %r2, %x3
  %pc = getelementptr i32, ptr %c, i64 %i
  store i32 %r3, ptr %pc
  %i.next = add i64 %i, 1
  %cmp = icmp ult i64 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
