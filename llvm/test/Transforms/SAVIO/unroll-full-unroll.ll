; RUN: opt -passes=savio -savio-port-info=%S/../../../../../src/port-info -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; The adi-like loop with a trip count of at most 3 (n & 3). The unroller lowers
; the factor to the maximum trip count and unrolls fully, which removes the loop
; and its phis: SAVIO used to go on deciding on the deleted loop and crashed. A
; loop that cannot run more than 8 times is left alone now.
; CHECK-NOT: unrolled by
; CHECK: define void @f
; CHECK: loop:
; CHECK: udiv i32
; CHECK-NOT: call <4 x i32> @llvm.savio.sv
target triple = "arm64-apple-macosx15.0.0"

define void @f(ptr %a, ptr %b, ptr %c, ptr %d, ptr %x, ptr %y, i64 %n) "target-cpu"="apple-m4" {
entry:
  %m = and i64 %n, 3
  %g = icmp sgt i64 %m, 0
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %pd = getelementptr inbounds i32, ptr %d, i64 %i
  %va = load i32, ptr %pa
  %vb = load i32, ptr %pb
  %vc = load i32, ptr %pc
  %vd = load i32, ptr %pd
  %db = or i32 %vb, 1
  %dd = or i32 %vd, 1
  %q0 = udiv i32 %va, %db
  %q1 = udiv i32 %vc, %dd
  %px = getelementptr inbounds i32, ptr %x, i64 %i
  %py = getelementptr inbounds i32, ptr %y, i64 %i
  store i32 %q0, ptr %px
  store i32 %q1, ptr %py
  %i.next = add nuw nsw i64 %i, 1
  %cmp = icmp slt i64 %i.next, %m
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
