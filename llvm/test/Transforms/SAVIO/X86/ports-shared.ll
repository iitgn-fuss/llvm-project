; REQUIRES: asserts
; RUN: opt -passes=savio-ir -savio-force-count=1 -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -savio-port-info=%S/Inputs/port-info -savio-cpu=emeraldrapids -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; Emerald Rapids vector multiply (p0,p1), vector shift (p0,p1) and vector ALU (p0,p1,p5) share ports, so the resource
; bound is taken over unions of port sets. Here 1.545 = (vec_mul 2.09 + vec_shift 1.0) / |{0,1}|, busiest class
; vec_mul. Per-class load/units (the old bound) would say vec_mul 2.09 / 2 = 1.045.
; CHECK: savio: estimate (copies 1, promoted): TRes 1.545{{0*e\+00}} (vec_mul) {{.*}} with512=0
target triple = "x86_64-unknown-linux-gnu"

define void @f(ptr noalias %k, ptr noalias %o, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pk = getelementptr inbounds i32, ptr %k, i64 %i
  %w = load <8 x i32>, ptr %pk, align 32
  %m = mul <8 x i32> %w, splat (i32 -2048144789)
  %s = lshr <8 x i32> %m, splat (i32 13)
  %x = xor <8 x i32> %s, %m
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store <8 x i32> %x, ptr %po, align 32
  %i.next = add nuw i64 %i, 8
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
