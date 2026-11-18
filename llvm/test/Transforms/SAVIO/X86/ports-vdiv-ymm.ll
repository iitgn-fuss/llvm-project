; REQUIRES: asserts
; RUN: opt -passes=savio-ir -savio-force-count=8 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-ports -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --implicit-check-not=with512=1 --implicit-check-not=fpdiv8.512
; clang's default attributes (no "prefer-vector-width"="512"): G = 4, and a <8 x i32> divide moved whole (two groups,
; fpdiv8) is emitted as two ymm divide sequences, not one zmm divide. It is costed with the key "fpdiv8.udiv v8i32"
; (the fixture has two fpdiv4 sequences there: vector=6 uops=12; the 512-bit key "fpdiv8.512.udiv v8i32" would give
; vector=3 uops=11) and is not a 512-bit region (with512=0, so the ports are those of 256-bit code).
; CHECK: savio: fpdiv-lanes of udiv <8 x i32> keys: 'udiv v8i32' 'fpdiv4.udiv v8i32'
; CHECK: savio: class loads (cycles, whole region): fp_div=1.600000e+01 vector=6.000000e+00 uops=1.200000e+01
; CHECK-NEXT: savio: estimate (copies 1, promoted 0 1): {{.*}} with512=0
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

attributes #0 = { "min-legal-vector-width"="0" }
