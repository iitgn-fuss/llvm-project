; REQUIRES: asserts
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-lane-widths -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=WIDTH
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=LANE
; As sc-lane-transfers-v8i32.ll for <16 x i32>: 4 vmovd + 12 vpinsrd + 2 vinserti32x4 + 1 vinserti64x4 = 31 cycles, latency 2 + 12*2/4 + 2 join levels * 3 = 14; extract 4 vmovd + 12 vpextrd + 3 vextracti = 31 cycles, latency 3 + 3 = 6 (all chunks fetched straight from the source). Untyped: 16 x the lane key.
; WIDTH: savio: transfer lane_extract v16i32 per-width: uops 31 latency 6 uses vector 3.100000e+01
; WIDTH: savio: transfer lane_insert v16i32 per-width: uops 31 latency 14 uses vector 3.100000e+01
; WIDTH-NOT: per-lane
; LANE: savio: transfer lane_extract per-lane x16: uops 16 latency 3 uses vector 4.800000e+01
; LANE: savio: transfer lane_insert per-lane x16: uops 16 latency 32 uses vector 4.800000e+01
; LANE-NOT: per-width
target triple = "x86_64-unknown-linux-gnu"

declare i32 @llvm.vector.reduce.add.v16i32(<16 x i32>)

define i32 @f(ptr noalias %k, ptr noalias %m, ptr noalias %o, i64 %n) {
entry:
  br label %loop

loop:
  %index = phi i64 [ 0, %entry ], [ %index.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %pk = getelementptr inbounds nuw i32, ptr %k, i64 %index
  %a = load <16 x i32>, ptr %pk, align 32
  %pm = getelementptr inbounds nuw i32, ptr %m, i64 %index
  %b0 = load <16 x i32>, ptr %pm, align 32
  store <16 x i32> %b0, ptr %o, align 32
  %b = xor <16 x i32> %b0, splat (i32 7)
  %t = mul <16 x i32> %a, %b
  %r = mul <16 x i32> %t, splat (i32 3)
  %s = call i32 @llvm.vector.reduce.add.v16i32(<16 x i32> %r)
  %acc.next = add i32 %acc, %s
  %index.next = add nuw i64 %index, 16
  %done = icmp eq i64 %index.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i32 %acc.next
}
