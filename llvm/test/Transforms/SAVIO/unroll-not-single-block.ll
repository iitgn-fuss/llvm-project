; RUN: opt -passes=savio-ir -savio-multi-block-loops=false -savio-port-info=%S/../../../../../src/port-info -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s --implicit-check-not=llvm.savio --implicit-check-not=.epil --implicit-check-not="unrolled by"
; The adi-like body behind an `if` inside the loop: the loop is not a
; single-block innermost loop, so SAVIO does not form a region for the loop
; blocks and does not unroll it. The pass leaves the loop alone.
; CHECK: define void @f
; CHECK: loop:
; CHECK: then:
; CHECK: latch:
target triple = "arm64-apple-macosx15.0.0"

define void @f(ptr %a, ptr %b, ptr %c, ptr %d, ptr %x, ptr %y, i64 %n) "target-cpu"="apple-m4" {
entry:
  %g = icmp sgt i64 %n, 0
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %va = load i32, ptr %pa
  %odd = and i32 %va, 1
  %t = icmp eq i32 %odd, 0
  br i1 %t, label %then, label %latch

then:
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %pd = getelementptr inbounds i32, ptr %d, i64 %i
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
  br label %latch

latch:
  %i.next = add nuw nsw i64 %i, 1
  %cmp = icmp slt i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
