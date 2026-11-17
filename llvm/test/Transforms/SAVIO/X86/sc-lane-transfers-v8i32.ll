; REQUIRES: asserts
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-lane-widths -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=WIDTH
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-lane-widths-only -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=WIDTH
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=LANE
; A Scalarized <8 x i32> group with a non-free input (%b: a vector computed in the loop) and a live-out that is
; not a store (%r, reduced). With the per-width keys lane_extract v8i32 / lane_insert v8i32 each boundary is one
; transfer per vector, charged the sum of its opcodes (fixture test-avx512-lane-widths): 2 vmovd + 6 vpinsrd +
; 1 vinserti32x4 = 15 vector cycles, 15 uops, latency 2 + 6*2/2 + 1*3 = 11; extract 2 vmovd + 6 vpextrd + 1
; vextracti32x4 = 15 cycles, 15 uops, latency 3 + 3 = 6. Without them (test-avx512) the untyped per-lane key is
; charged eight times, the insert chain's latency eight times too.
; WIDTH: savio: transfer lane_extract v8i32 per-width: uops 15 latency 6 uses vector 1.500000e+01
; WIDTH: savio: transfer lane_insert v8i32 per-width: uops 15 latency 11 uses vector 1.500000e+01
; WIDTH-NOT: per-lane
; LANE: savio: transfer lane_extract per-lane x8: uops 8 latency 3 uses vector 2.400000e+01
; LANE: savio: transfer lane_insert per-lane x8: uops 8 latency 16 uses vector 2.400000e+01
; LANE-NOT: per-width
target triple = "x86_64-unknown-linux-gnu"

declare i32 @llvm.vector.reduce.add.v8i32(<8 x i32>)

define i32 @f(ptr noalias %k, ptr noalias %m, ptr noalias %o, i64 %n) {
entry:
  br label %loop

loop:
  %index = phi i64 [ 0, %entry ], [ %index.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %pk = getelementptr inbounds nuw i32, ptr %k, i64 %index
  %a = load <8 x i32>, ptr %pk, align 32
  %pm = getelementptr inbounds nuw i32, ptr %m, i64 %index
  %b0 = load <8 x i32>, ptr %pm, align 32
  store <8 x i32> %b0, ptr %o, align 32
  %b = xor <8 x i32> %b0, splat (i32 7)
  %t = mul <8 x i32> %a, %b
  %r = mul <8 x i32> %t, splat (i32 3)
  %s = call i32 @llvm.vector.reduce.add.v8i32(<8 x i32> %r)
  %acc.next = add i32 %acc, %s
  %index.next = add nuw i64 %index, 8
  %done = icmp eq i64 %index.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i32 %acc.next
}
