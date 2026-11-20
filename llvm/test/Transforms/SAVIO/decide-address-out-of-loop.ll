; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v-wide -savio-force -pass-remarks=savio -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; REQUIRES: asserts
; Scoped address check: out-of-loop uses of a value in a getelementptr must not
; disqualify in-loop computation from singleton vectorization, because live-outs
; are transferred to GPRs at region exit.
target triple = "arm64-apple-macos"

; CHECK: savio: group of 1 ops (slice, class 0) in %loop
; CHECK: liveout: %s
; CHECK: remark: {{.*}}T 1.00 -> {{.*}}, 1/1 of slice 0
define void @test_out_of_loop_gep(ptr %a, ptr %b, ptr %c, ptr %out) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr i32, ptr %a, i32 %i
  %x = load i32, ptr %pa
  %pb = getelementptr i32, ptr %b, i32 %i
  %y = load i32, ptr %pb
  %s = add i32 %x, %y
  %pc = getelementptr i32, ptr %c, i32 %i
  store i32 %s, ptr %pc
  %i.next = add i32 %i, 1
  %cmp = icmp ult i32 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ; %s is used out-of-loop in a GEP calculation here.
  %s.ext = sext i32 %s to i64
  %pout = getelementptr i32, ptr %out, i64 %s.ext
  store i32 42, ptr %pout
  ret void
}
