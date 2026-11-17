; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1vc -pass-remarks=savio -S %s 2>&1 | FileCheck %s --check-prefix=TABLE
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1vn -pass-remarks=savio -S %s 2>&1 | FileCheck %s --check-prefix=NOTABLE
; Four independent urem slices on the fixture machines test2s1vc / test2s1vn,
; which have the same sv.urem v4i32 sequence entry (8 vector cycles) but only
; test2s1vc gives it a concurrency table (8, 4, 3 cycles per instance at
; 1, 2, 4 instances in flight). One promoted instance costs 8 (T 12); two
; instances in flight cost 4 each (T 8). Without the table the second
; promotion makes things worse (2 x 8 vector cycles), so only one is taken.
; This pins the wiring in the decision: the table reaches the cost nodes and is
; applied before the estimate.
; TABLE: T 16.00 -> 8.00
; TABLE-COUNT-2: call <4 x i32> @llvm.savio.sv.urem.v4i32
; TABLE-NOT: call <4 x i32> @llvm.savio.sv.urem.v4i32
; NOTABLE: T 16.00 -> 12.00
; NOTABLE-COUNT-1: call <4 x i32> @llvm.savio.sv.urem.v4i32
; NOTABLE-NOT: call <4 x i32> @llvm.savio.sv.urem.v4i32
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
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
