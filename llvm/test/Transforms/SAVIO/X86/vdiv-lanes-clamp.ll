; RUN: opt -passes=savio-ir,verify -savio-force-count=8 -mtriple=x86_64-unknown-linux-gnu -mcpu=haswell -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -debug-only=savio -S %s 2>&1 | FileCheck %s
; test-avx512 is a 512-bit vector machine but the target (haswell) has 256-bit vector registers: the lane group is
; clamped to 256 / 64 = 4. A <8 x i32> divide moves at most fpdiv8 as two groups, never fpdiv16, and the 512-bit
; <16 x i32> divide is not a candidate at all (no key of it is looked up).
; CHECK-NOT: v16i32
; CHECK-NOT: fpdiv16
; CHECK: call <8 x i32> @llvm.savio.fpdiv8.udiv.v8i32(
; CHECK-NOT: llvm.savio.fpdiv16
; CHECK-NOT: v16i32
; REQUIRES: asserts
define void @f(ptr %b, ptr %c, ptr %o, ptr %b2, ptr %c2, ptr %o2, i64 %n) {
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
  %pb2 = getelementptr inbounds i32, ptr %b2, i64 %i
  %pc2 = getelementptr inbounds i32, ptr %c2, i64 %i
  %wb = load <16 x i32>, ptr %pb2
  %wc = load <16 x i32>, ptr %pc2
  %q = udiv <16 x i32> %wb, %wc
  %po2 = getelementptr inbounds i32, ptr %o2, i64 %i
  store <16 x i32> %q, ptr %po2
  %i.next = add nuw i64 %i, 16
  %done = icmp uge i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
