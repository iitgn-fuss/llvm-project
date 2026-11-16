; RUN: opt -passes=savio -savio-port-info=%S/../../../../../src/port-info -pass-remarks=savio -S %s 2>&1 | FileCheck %s
; The adi-like loop inside an outer loop, with the last quotient used after
; the inner loop without an LCSSA phi (as a function can look at the end of
; the pipeline). The unroller needs the whole nest in LCSSA form, so SAVIO
; puts it there first; the module must still verify.
; CHECK: unrolled by 4
; CHECK: loop.epil:
target triple = "arm64-apple-macosx15.0.0"

define void @f(ptr %a, ptr %b, ptr %c, ptr %d, ptr %x, ptr %y, ptr %z, i64 %n, i64 %m) "target-cpu"="apple-m4" {
entry:
  %g = icmp sgt i64 %n, 0
  %h = icmp sgt i64 %m, 0
  %gh = and i1 %g, %h
  br i1 %gh, label %outer, label %done

outer:
  %j = phi i64 [ 0, %entry ], [ %j.next, %after ]
  br label %loop

loop:
  %i = phi i64 [ 0, %outer ], [ %i.next, %loop ]
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
  %cmp = icmp slt i64 %i.next, %n
  br i1 %cmp, label %loop, label %after

after:
  %pz = getelementptr inbounds i32, ptr %z, i64 %j
  store i32 %q0, ptr %pz
  %j.next = add nuw nsw i64 %j, 1
  %cj = icmp slt i64 %j.next, %m
  br i1 %cj, label %outer, label %done

done:
  ret void
}
