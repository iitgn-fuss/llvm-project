; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks=savio -pass-remarks-missed=savio -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -savio-force -pass-remarks=savio -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=FORCE
; REQUIRES: asserts
; Addresses stay in GPRs: an index computation and an induction increment whose
; phi reaches addressing (directly, or only through the in-scope mul in @g) are
; not candidates. Only %s (not feeding an address)
; forms a group, in @f; @g has no candidates at all and emits nothing.
target triple = "arm64-apple-macos"

; CHECK-COUNT-1: savio: group of
; CHECK-NOT: savio: group of
; CHECK: remark: {{.*}}T 0.50 (resource)
; CHECK-NOT: remark:
; FORCE-COUNT-1: savio: group of
; FORCE-NOT: savio: group of
; FORCE: remark: {{.*}}T 0.50 -> 1.00 (resource)
; FORCE-NOT: remark:
define void @f(ptr %a, ptr %b, ptr %c) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %k = mul i32 %i, 3
  %kx = sext i32 %k to i64
  %pa = getelementptr i32, ptr %a, i64 %kx
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
  ret void
}

define void @g(ptr %a) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %k = mul i32 %i, 3
  %kx = zext i32 %k to i64
  %pa = getelementptr i32, ptr %a, i64 %kx
  store i32 0, ptr %pa
  %i.next = add i32 %i, 1
  %cmp = icmp ult i32 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
