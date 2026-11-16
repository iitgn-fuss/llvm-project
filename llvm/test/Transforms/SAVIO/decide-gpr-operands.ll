; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v-wide -pass-remarks=savio -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s
; The same adds, in a loop on the wide-dispatch fixture, but operands come from opaque calls and results feed calls: each
; group needs GprToVec and VecToGpr transfers, so nothing is promoted.
; CHECK: remark: {{.*}}T 3.00 (resource)
; CHECK-NOT: remark:
target triple = "arm64-apple-macos"

declare i32 @src()
declare void @sink(i32)

define void @f() {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %v0 = call i32 @src()
  %v1 = call i32 @src()
  %v2 = call i32 @src()
  %v3 = call i32 @src()
  %v4 = call i32 @src()
  %v5 = call i32 @src()
  %v6 = call i32 @src()
  %v7 = call i32 @src()
  %v8 = call i32 @src()
  %v9 = call i32 @src()
  %v10 = call i32 @src()
  %v11 = call i32 @src()
  %s0 = add i32 %v0, %v1
  call void @sink(i32 %s0)
  %s1 = add i32 %v2, %v3
  call void @sink(i32 %s1)
  %s2 = add i32 %v4, %v5
  call void @sink(i32 %s2)
  %s3 = add i32 %v6, %v7
  call void @sink(i32 %s3)
  %s4 = add i32 %v8, %v9
  call void @sink(i32 %s4)
  %s5 = add i32 %v10, %v11
  call void @sink(i32 %s5)
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
