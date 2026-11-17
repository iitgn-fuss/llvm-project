; REQUIRES: asserts
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-lane-widths -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=WIDTH
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=LANE
; As sc-lane-transfers-v8i32.ll for <4 x i64>: i64 lanes use their own opcodes: insert 4 vmovq + 2 vpunpcklqdq + 1 vinserti32x4 = 7 cycles, latency 2 + 2*1/2 + 3 = 6; extract 2 vmovq + 2 vpextrq + 1 vextracti32x4 = 7 cycles, latency 3 + 3 = 6. Untyped: 4 x the lane key.
; WIDTH: savio: transfer lane_extract v4i64 per-width: uops 7 latency 6 uses vector 7.000000e+00
; WIDTH: savio: transfer lane_insert v4i64 per-width: uops 7 latency 6 uses vector 7.000000e+00
; WIDTH-NOT: per-lane
; LANE: savio: transfer lane_extract per-lane x4: uops 4 latency 3 uses vector 1.200000e+01
; LANE: savio: transfer lane_insert per-lane x4: uops 4 latency 8 uses vector 1.200000e+01
; LANE-NOT: per-width
target triple = "x86_64-unknown-linux-gnu"

declare i64 @llvm.vector.reduce.add.v4i64(<4 x i64>)

define i64 @f(ptr noalias %k, ptr noalias %m, ptr noalias %o, i64 %n) {
entry:
  br label %loop

loop:
  %index = phi i64 [ 0, %entry ], [ %index.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  %pk = getelementptr inbounds nuw i64, ptr %k, i64 %index
  %a = load <4 x i64>, ptr %pk, align 32
  %pm = getelementptr inbounds nuw i64, ptr %m, i64 %index
  %b0 = load <4 x i64>, ptr %pm, align 32
  store <4 x i64> %b0, ptr %o, align 32
  %b = xor <4 x i64> %b0, splat (i64 7)
  %t = mul <4 x i64> %a, %b
  %r = mul <4 x i64> %t, splat (i64 3)
  %s = call i64 @llvm.vector.reduce.add.v4i64(<4 x i64> %r)
  %acc.next = add i64 %acc, %s
  %index.next = add nuw i64 %index, 4
  %done = icmp eq i64 %index.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i64 %acc.next
}
