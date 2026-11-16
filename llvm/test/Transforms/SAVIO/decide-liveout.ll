; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v-wide -pass-remarks=savio -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s
; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v-wide -savio-force -pass-remarks=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=FORCE
; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v-wide -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=DBG
; REQUIRES: asserts
; Six adds in a loop, %s0 also used after the loop. Forced: all six, and the
; vector class carries six adds plus the one VecToGpr for %s0, so T = 7.
; Unforced: %s0 ties with the others for the first step (T 2.5: vector 2 with
; its VecToGpr, scalar 2.5) and is taken; a second step would only reach T 2.0
; if the VecToGpr were free, so exactly one group is promoted.
; CHECK-COUNT-1: remark: {{.*}}T 3.00 -> 2.50 (resource)
; CHECK-NOT: remark:
; FORCE-COUNT-6: remark: {{.*}}T 3.00 -> 7.00 (resource)
; FORCE-NOT: remark:
; DBG: liveout: %s0
target triple = "arm64-apple-macos"

define i32 @f(ptr %p, ptr %q) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %ia0 = add i64 %i, 0
  %pa0 = getelementptr i32, ptr %p, i64 %ia0
  %x0 = load i32, ptr %pa0
  %pb0 = getelementptr i32, ptr %q, i64 %ia0
  %y0 = load i32, ptr %pb0
  %s0 = add i32 %x0, %y0
  %pc0 = getelementptr i32, ptr %p, i64 %ia0
  store i32 %s0, ptr %pc0
  %ia1 = add i64 %i, 1
  %pa1 = getelementptr i32, ptr %p, i64 %ia1
  %x1 = load i32, ptr %pa1
  %pb1 = getelementptr i32, ptr %q, i64 %ia1
  %y1 = load i32, ptr %pb1
  %s1 = add i32 %x1, %y1
  %pc1 = getelementptr i32, ptr %p, i64 %ia1
  store i32 %s1, ptr %pc1
  %ia2 = add i64 %i, 2
  %pa2 = getelementptr i32, ptr %p, i64 %ia2
  %x2 = load i32, ptr %pa2
  %pb2 = getelementptr i32, ptr %q, i64 %ia2
  %y2 = load i32, ptr %pb2
  %s2 = add i32 %x2, %y2
  %pc2 = getelementptr i32, ptr %p, i64 %ia2
  store i32 %s2, ptr %pc2
  %ia3 = add i64 %i, 3
  %pa3 = getelementptr i32, ptr %p, i64 %ia3
  %x3 = load i32, ptr %pa3
  %pb3 = getelementptr i32, ptr %q, i64 %ia3
  %y3 = load i32, ptr %pb3
  %s3 = add i32 %x3, %y3
  %pc3 = getelementptr i32, ptr %p, i64 %ia3
  store i32 %s3, ptr %pc3
  %ia4 = add i64 %i, 4
  %pa4 = getelementptr i32, ptr %p, i64 %ia4
  %x4 = load i32, ptr %pa4
  %pb4 = getelementptr i32, ptr %q, i64 %ia4
  %y4 = load i32, ptr %pb4
  %s4 = add i32 %x4, %y4
  %pc4 = getelementptr i32, ptr %p, i64 %ia4
  store i32 %s4, ptr %pc4
  %ia5 = add i64 %i, 5
  %pa5 = getelementptr i32, ptr %p, i64 %ia5
  %x5 = load i32, ptr %pa5
  %pb5 = getelementptr i32, ptr %q, i64 %ia5
  %y5 = load i32, ptr %pb5
  %s5 = add i32 %x5, %y5
  %pc5 = getelementptr i32, ptr %p, i64 %ia5
  store i32 %s5, ptr %pc5
  %i.next = add i64 %i, 6
  %cmp = icmp ult i64 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %s0
}
