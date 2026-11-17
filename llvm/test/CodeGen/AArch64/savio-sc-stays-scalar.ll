; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/../../Transforms/SAVIO/Inputs/port-info -savio-cpu=test3scv -S %s | llc -mtriple=arm64-apple-macos -mcpu=apple-m4 | FileCheck %s --implicit-check-not=.4s
; A Scalarized copy, from the loads to the stores, must compile to scalar ldp/ldr w, add w, eor w and stp/str w (no .4s operation anywhere): the DAG combiner does
; not merge the per-lane scalar operations back into a vector.
; CHECK-LABEL: _f:
; CHECK: ldp w
; CHECK: add w
; CHECK: eor w
; CHECK: stp w
target triple = "arm64-apple-macos"

define void @f(ptr noalias %a, ptr noalias %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %x = load <4 x i32>, ptr %pa, align 4
  %s = add <4 x i32> %x, splat (i32 3)
  %t = xor <4 x i32> %s, splat (i32 5)
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  store <4 x i32> %t, ptr %pb, align 4
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
