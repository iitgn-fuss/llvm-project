; RUN: opt -passes=savio -savio-port-info=%S/../../../../../src/port-info -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; The urem_store_x4 body (four independent (b*M) % ((c&1023)+1) results
; stored, no recurrence) on apple-m4 with the measured sequence entries
; (sv.urem v4i32 with its concurrency table, window 55 uops; see
; results/savio-pass/m4-calibration.md). The remark reports T per iteration
; in cycles: 8.04 unchanged (resource bound, four scalar urem on int_div).
; With k instances moved the calibrated model gives T = 6.03 (k=1), 6.60
; (k=2), 8.57 (k=3), so exactly one of four moves: a second instance's
; sequence cost (about 2 x 3.8 vec_alu cycles) outweighs the int_div it frees.
; CHECK: remark: {{.*}}: T 8.04 -> 6.03 (resource), 1/4 of slice {{[0-9]+}}
; CHECK-COUNT-1: call <4 x i32> @llvm.savio.sv.urem.v4i32
; CHECK-NOT: call <4 x i32> @llvm.savio.sv.urem.v4i32
target triple = "arm64-apple-macosx15.0.0"

define void @f(ptr %b, ptr %c, ptr %o, i64 %n) "target-cpu"="apple-m4" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i0 = add nuw i64 %i, 0
  %pb0 = getelementptr inbounds i32, ptr %b, i64 %i0
  %pc0 = getelementptr inbounds i32, ptr %c, i64 %i0
  %b0 = load i32, ptr %pb0
  %c0 = load i32, ptr %pc0
  %m0 = mul i32 %b0, -1640531535
  %k0 = and i32 %c0, 1023
  %d0 = add i32 %k0, 1
  %r0 = urem i32 %m0, %d0
  %po0 = getelementptr inbounds i32, ptr %o, i64 %i0
  store i32 %r0, ptr %po0
  %i1 = add nuw i64 %i, 1
  %pb1 = getelementptr inbounds i32, ptr %b, i64 %i1
  %pc1 = getelementptr inbounds i32, ptr %c, i64 %i1
  %b1 = load i32, ptr %pb1
  %c1 = load i32, ptr %pc1
  %m1 = mul i32 %b1, -1640531535
  %k1 = and i32 %c1, 1023
  %d1 = add i32 %k1, 1
  %r1 = urem i32 %m1, %d1
  %po1 = getelementptr inbounds i32, ptr %o, i64 %i1
  store i32 %r1, ptr %po1
  %i2 = add nuw i64 %i, 2
  %pb2 = getelementptr inbounds i32, ptr %b, i64 %i2
  %pc2 = getelementptr inbounds i32, ptr %c, i64 %i2
  %b2 = load i32, ptr %pb2
  %c2 = load i32, ptr %pc2
  %m2 = mul i32 %b2, -1640531535
  %k2 = and i32 %c2, 1023
  %d2 = add i32 %k2, 1
  %r2 = urem i32 %m2, %d2
  %po2 = getelementptr inbounds i32, ptr %o, i64 %i2
  store i32 %r2, ptr %po2
  %i3 = add nuw i64 %i, 3
  %pb3 = getelementptr inbounds i32, ptr %b, i64 %i3
  %pc3 = getelementptr inbounds i32, ptr %c, i64 %i3
  %b3 = load i32, ptr %pb3
  %c3 = load i32, ptr %pc3
  %m3 = mul i32 %b3, -1640531535
  %k3 = and i32 %c3, 1023
  %d3 = add i32 %k3, 1
  %r3 = urem i32 %m3, %d3
  %po3 = getelementptr inbounds i32, ptr %o, i64 %i3
  store i32 %r3, ptr %po3
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
