; REQUIRES: asserts
; RUN: opt -passes=savio-ir -savio-force-count=8 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-ports -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; The fpdiv8 sequence of a <8 x i32> divide (G = 8, "prefer-vector-width"="512") runs on 512-bit registers, so the
; estimate that moves all eight lanes has with512=1; the all-integer-divide estimate of the same <8 x i32> (256-bit)
; region has with512=0.
; CHECK: savio: estimate (copies 1, promoted): {{.*}} with512=0
; CHECK: savio: estimate (copies 1, promoted 0): {{.*}} with512=1
; Eight lanes without AVX-512: both groups, one fpdiv8.
define void @f(ptr %b, ptr %c, ptr %o, i64 %n) #0 {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %vb = load <8 x i32>, ptr %pb
  %vc = load <8 x i32>, ptr %pc
  %r = udiv <8 x i32> %vb, %vc
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store <8 x i32> %r, ptr %po
  %i.next = add nuw i64 %i, 8
  %done = icmp uge i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

attributes #0 = { "prefer-vector-width"="512" "min-legal-vector-width"="0" }
