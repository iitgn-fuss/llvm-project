; RUN: opt -passes=savio-ir -savio-port-info=%S/../../../../../src/port-info -savio-cpu=emeraldrapids -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; A cost-driven decision should not rewrite a cold standalone remainder block.
; Its vector multiply is valid and the entry block is hot, but the branch
; metadata makes the remainder block much less frequent than the entry block.
target triple = "x86_64-unknown-linux-gnu"

define void @cold_remainder(ptr %p, i64 %n) "target-cpu"="emeraldrapids" {
entry:
  %hot = icmp slt i64 %n, 0
  br i1 %hot, label %hot_block, label %cold_block, !prof !0

hot_block:
  ret void

cold_block:
  %a = load <4 x i32>, ptr %p, align 16
  %m = mul <4 x i32> %a, <i32 3, i32 5, i32 7, i32 9>
  store <4 x i32> %m, ptr %p, align 16
  ret void
}

; CHECK: define void @cold_remainder
; CHECK-NOT: llvm.savio
; CHECK-NOT: remark:

!0 = !{!"branch_weights", i32 1000, i32 1}
