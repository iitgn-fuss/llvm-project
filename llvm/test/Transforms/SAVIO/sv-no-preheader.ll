; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force. The loop has two outside predecessors, so no
; preheader: the invariant %k cannot be hoisted and is charged and inserted
; inside the loop.
target triple = "arm64-apple-macos"

define void @f(ptr %p, i64 %k64, i1 %c) {
; CHECK-LABEL: @f(
; CHECK:       loop:
; CHECK:         load i32
; CHECK:         insertelement <4 x i32> poison, i32 %k, i32 0
; CHECK:         call <4 x i32> @llvm.savio.sv.add.v4i32(
entry:
  %k = trunc i64 %k64 to i32
  br i1 %c, label %a, label %b

a:
  br label %loop

b:
  br label %loop

loop:
  %i = phi i64 [ 0, %a ], [ 1, %b ], [ %i.next, %loop ]
  %pa = getelementptr i32, ptr %p, i64 %i
  %x = load i32, ptr %pa
  %y = add i32 %x, %k
  store i32 %y, ptr %pa
  %i.next = add i64 %i, 1
  %cmp = icmp ult i64 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
