; RUN: opt -passes=savio -savio-port-info=%S/../../../../../src/port-info -S %s | FileCheck %s
; RUN: opt -passes=savio -savio-port-info=%S/../../../../../src/port-info -S %s | opt -passes=verify -disable-output
; Default cost model (no -savio-force) on apple-m4: the divides are promoted,
; and the argument %p, used by both, is a loop-invariant input used twice.
target triple = "arm64-apple-macosx15.0.0"

define void @h(ptr %src, ptr %dst, i32 %p, i64 %n) #0 {
; CHECK-LABEL: @h(
; CHECK:       loop:
; CHECK-COUNT-2: call <4 x i32> @llvm.savio.sv.urem.v4i32(
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %src, i64 %i
  %a = load i32, ptr %pa
  %pb = getelementptr inbounds i32, ptr %dst, i64 %i
  %b = load i32, ptr %pb
  %x = urem i32 %a, %p
  %y = urem i32 %b, %p
  %s = add i32 %x, %y
  store i32 %s, ptr %pb
  %i.next = add nuw i64 %i, 1
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}
attributes #0 = { "target-cpu"="apple-m4" }
