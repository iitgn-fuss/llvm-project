; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force: the add group is promoted. The loop-invariant
; operand %k is inserted once in the preheader, not in the loop.
target triple = "arm64-apple-macos"

define void @f(ptr %p, i64 %k64) {
; CHECK-LABEL: @f(
; CHECK:       entry:
; CHECK:         [[K:%.*]] = trunc i64 %k64 to i32
; CHECK-NEXT:    [[VK:%.*]] = insertelement <4 x i32> poison, i32 [[K]], i32 0
; CHECK-NEXT:    br label %loop
; CHECK:       loop:
; CHECK-NOT:     [[K]]
; CHECK:         call <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32> {{%.*}}, <4 x i32> [[VK]])
entry:
  %k = trunc i64 %k64 to i32
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
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
