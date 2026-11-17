; RUN: not --crash opt -passes=savio-ir -savio-force-count=3 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -mattr=-avx512f -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s 2>&1 | FileCheck %s
; With AVX-512 off the lane group of a vector divide is 4 (256-bit registers): -savio-force-count counts lanes and
; must be a multiple of it.
; CHECK: -savio-force-count=3 must be a multiple of the lane group 4
define void @f(ptr %b, ptr %c, ptr %o, i64 %n) {
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
