; RUN: opt -passes=savio -savio-force-count=2 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s --check-prefix=K2
; RUN: opt -passes=savio -savio-force-count=0 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s --check-prefix=K0
; RUN: opt -passes=savio -savio-force-count=9 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s --check-prefix=K4
; RUN: opt -passes=savio -savio-force-count=2 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Four interchangeable urem slices (the same operations and input kinds) and
; one lone add slice. -savio-force-count=k promotes the first k instances of the
; largest slice class in program order and nothing else.
; K2-COUNT-2: call <4 x i32> @llvm.savio.sv.urem
; K2-NOT: call <4 x i32> @llvm.savio.sv.urem
; K2-NOT: call <4 x i32> @llvm.savio.sv.add
; K2: urem i32
; K2: urem i32
; K0-NOT: llvm.savio.sv
; K4-COUNT-4: call <4 x i32> @llvm.savio.sv.urem
; K4-NOT: call <4 x i32> @llvm.savio.sv.urem
; K4-NOT: llvm.savio.sv.add
target triple = "arm64-apple-macos"

define void @f(ptr %b, ptr %c, ptr %o, ptr %q, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %b0 = load i32, ptr %pb
  %c0 = load i32, ptr %pc
  %r0 = urem i32 %b0, %c0
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store i32 %r0, ptr %po
  %i1 = or disjoint i64 %i, 1
  %pb1 = getelementptr inbounds i32, ptr %b, i64 %i1
  %pc1 = getelementptr inbounds i32, ptr %c, i64 %i1
  %b1 = load i32, ptr %pb1
  %c1 = load i32, ptr %pc1
  %r1 = urem i32 %b1, %c1
  %po1 = getelementptr inbounds i32, ptr %o, i64 %i1
  store i32 %r1, ptr %po1
  %i2 = or disjoint i64 %i, 2
  %pb2 = getelementptr inbounds i32, ptr %b, i64 %i2
  %pc2 = getelementptr inbounds i32, ptr %c, i64 %i2
  %b2 = load i32, ptr %pb2
  %c2 = load i32, ptr %pc2
  %r2 = urem i32 %b2, %c2
  %po2 = getelementptr inbounds i32, ptr %o, i64 %i2
  store i32 %r2, ptr %po2
  %i3 = or disjoint i64 %i, 3
  %pb3 = getelementptr inbounds i32, ptr %b, i64 %i3
  %pc3 = getelementptr inbounds i32, ptr %c, i64 %i3
  %b3 = load i32, ptr %pb3
  %c3 = load i32, ptr %pc3
  %r3 = urem i32 %b3, %c3
  %po3 = getelementptr inbounds i32, ptr %o, i64 %i3
  store i32 %r3, ptr %po3
  %q0 = load i32, ptr %q
  %a0 = add i32 %q0, 7
  store i32 %a0, ptr %q
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
