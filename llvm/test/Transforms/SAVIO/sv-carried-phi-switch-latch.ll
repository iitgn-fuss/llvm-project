; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; The latch is a switch with two cases to the header: the carried phi has two
; incoming edges from %loop, and so does its vector form.
target triple = "arm64-apple-macos"

define i32 @f(ptr %p, i32 %init) {
; CHECK-LABEL: @f(
; CHECK:         phi <4 x i32> [ {{%.*}}, %entry ], [ [[NEXT:%.*]], %loop ], [ [[NEXT]], %loop ]
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ], [ %i.next, %loop ]
  %acc = phi i32 [ %init, %entry ], [ %acc.next, %loop ], [ %acc.next, %loop ]
  %pa = getelementptr i32, ptr %p, i64 %i
  %x = load i32, ptr %pa
  %acc.next = add i32 %acc, %x
  %i.next = add i64 %i, 1
  %k = and i64 %i.next, 3
  switch i64 %k, label %exit [
    i64 0, label %loop
    i64 1, label %loop
  ]

exit:
  %r = phi i32 [ %acc.next, %loop ]
  ret i32 %r
}
