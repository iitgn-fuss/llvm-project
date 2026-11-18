; REQUIRES: asserts
; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-no-v16i32 -debug-only=savio -S %s 2>&1 | FileCheck %s --implicit-check-not="group of"
; A <16 x i32> loop (512-bit: a Scalarize candidate by width, a udiv lane-group candidate with 512-bit registers) with
; every v16i32 key removed from the port-info: no candidate is formed (no "group of" line anywhere in the debug output:
; --implicit-check-not, because the debug output precedes the IR), the IR is unchanged, no crash.
; CHECK-DAG: savio: scalarize of mul <16 x i32> not available: missing 'mul v16i32'
; CHECK-DAG: savio: fpdiv-lanes of udiv <16 x i32> not available: missing 'udiv v16i32'
; CHECK-LABEL: define void @f(
; CHECK: load <16 x i32>
; CHECK: load <16 x i32>
; CHECK-NEXT: %m = mul <16 x i32>
; CHECK-NEXT: %q = udiv <16 x i32>
; CHECK-NOT: llvm.savio
define void @f(ptr %b, ptr %c, ptr %o, ptr %o2, i64 %n) "prefer-vector-width"="512" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %vb = load <16 x i32>, ptr %pb
  %vc = load <16 x i32>, ptr %pc
  %m = mul <16 x i32> %vb, %vc
  %q = udiv <16 x i32> %vb, %vc
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store <16 x i32> %m, ptr %po
  %po2 = getelementptr inbounds i32, ptr %o2, i64 %i
  store <16 x i32> %q, ptr %po2
  %i.next = add nuw i64 %i, 16
  %done = icmp uge i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
