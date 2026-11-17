; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks=savio -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=NARROW
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v-wide -pass-remarks=savio -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=WIDE
; The straight-line input decide-scalar-heavy.ll had before the whole-region
; model: 12 loads, 6 adds, 6 stores, none in a loop. With every instruction in the
; model nothing is promoted any more (the old verdict was two groups, T 3.00 ->
; 2.00), because the scalar resource class (T 3.00) no longer limits:
; - outside a loop the recurrence bound is the critical path (gep, load, add,
;   store), 6.00, whatever the machine;
; - with width 4 (test2s1v) the 24 micro-ops (the loads and stores count too) give
;   a dispatch bound of 6.00 as well, tying with it; the tie resolves to
;   recurrence. With width 64 dispatch is 0.38 and the critical path alone limits.
; NARROW: remark: {{.*}}T 6.00 (recurrence)
; NARROW-NOT: remark:
; WIDE: remark: {{.*}}T 6.00 (recurrence)
; WIDE-NOT: remark:
target triple = "arm64-apple-macos"

define void @f(ptr %p, ptr %q) {
  %p0 = getelementptr i32, ptr %p, i64 0
  %v0 = load i32, ptr %p0
  %p1 = getelementptr i32, ptr %p, i64 1
  %v1 = load i32, ptr %p1
  %p2 = getelementptr i32, ptr %p, i64 2
  %v2 = load i32, ptr %p2
  %p3 = getelementptr i32, ptr %p, i64 3
  %v3 = load i32, ptr %p3
  %p4 = getelementptr i32, ptr %p, i64 4
  %v4 = load i32, ptr %p4
  %p5 = getelementptr i32, ptr %p, i64 5
  %v5 = load i32, ptr %p5
  %p6 = getelementptr i32, ptr %p, i64 6
  %v6 = load i32, ptr %p6
  %p7 = getelementptr i32, ptr %p, i64 7
  %v7 = load i32, ptr %p7
  %p8 = getelementptr i32, ptr %p, i64 8
  %v8 = load i32, ptr %p8
  %p9 = getelementptr i32, ptr %p, i64 9
  %v9 = load i32, ptr %p9
  %p10 = getelementptr i32, ptr %p, i64 10
  %v10 = load i32, ptr %p10
  %p11 = getelementptr i32, ptr %p, i64 11
  %v11 = load i32, ptr %p11
  %s0 = add i32 %v0, %v1
  %q0 = getelementptr i32, ptr %q, i64 0
  store i32 %s0, ptr %q0
  %s1 = add i32 %v2, %v3
  %q1 = getelementptr i32, ptr %q, i64 1
  store i32 %s1, ptr %q1
  %s2 = add i32 %v4, %v5
  %q2 = getelementptr i32, ptr %q, i64 2
  store i32 %s2, ptr %q2
  %s3 = add i32 %v6, %v7
  %q3 = getelementptr i32, ptr %q, i64 3
  store i32 %s3, ptr %q3
  %s4 = add i32 %v8, %v9
  %q4 = getelementptr i32, ptr %q, i64 4
  store i32 %s4, ptr %q4
  %s5 = add i32 %v10, %v11
  %q5 = getelementptr i32, ptr %q, i64 5
  store i32 %s5, ptr %q5
  ret void
}
