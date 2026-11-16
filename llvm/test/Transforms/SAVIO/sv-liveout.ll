; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force: the add group is promoted. %s is stored inside the
; loop and returned after it: exactly one extractelement, and both the store
; and the return use it.
target triple = "arm64-apple-macos"

define i32 @f(ptr %p, ptr %q) {
; CHECK-LABEL: @f(
; CHECK:       loop:
; CHECK:         [[S:%.*]] = call <4 x i32> @llvm.savio.sv.add.v4i32(
; CHECK-NEXT:    [[E:%.*]] = extractelement <4 x i32> [[S]], i32 0
; CHECK:         store i32 [[E]], ptr
; CHECK-NOT:     extractelement
; CHECK:       exit:
; CHECK-NEXT:    ret i32 [[E]]
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr i32, ptr %p, i64 %i
  %x = load i32, ptr %pa
  %pb = getelementptr i32, ptr %q, i64 %i
  %y = load i32, ptr %pb
  %s = add i32 %x, %y
  store i32 %s, ptr %pa
  %i.next = add i64 %i, 1
  %cmp = icmp ult i64 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %s
}
