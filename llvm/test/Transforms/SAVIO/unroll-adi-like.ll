; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1vc -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; adi's shape: two independent divides per element (x[i] = a[i] / b[i] and
; y[i] = c[i] / d[i]), no recurrence through the divides, a trip count that
; ScalarEvolution can compute. Two instances of the divide slice are already in
; the body; the divide unit is the bottleneck, so the pass unrolls the loop to
; reach a better fraction and moves some of the divides to the vector side.
; The unrolled loop keeps an epilogue loop for any trip count.
; The test CPU test2s1vc (the loop's loads and stores are not charged by it): T is
; 8.00 per original iteration unchanged (two divides on the divider); unrolled by
; 4 there are 8 instances of the divide slice and moving 3 gives T 5.00 per
; original iteration (the remark reports T per original iteration).
; (On apple-m4 the loop's four loads and two stores are charged since the
; out-of-group charge: the address units bound T at 2.55 and no unroll helps,
; see unroll-disabled.ll.)
; CHECK: remark: {{.*}}: T 8.00 -> 5.00 (resource), 3/8 of slice {{[0-9]+}}, unrolled by 4 (estimated 3/8 T 5.00)
; CHECK: define void @f
; The remainder count is n mod 4.
; CHECK: and i64 %n, 3
; CHECK: loop:
; CHECK-COUNT-3: call <4 x i32> @llvm.savio.sv.udiv.v4i32
; Exactly three: none between the third and the loop branch.
; CHECK-NOT: call <4 x i32> @llvm.savio.sv.udiv.v4i32
; CHECK: br i1 %niter.ncmp.3, label %loop, label %exit.loopexit.unr-lcssa
; The remainder loop is the original scalar body.
; CHECK: loop.epil:
; CHECK: udiv i32
; CHECK-NOT: call <4 x i32> @llvm.savio
target triple = "arm64-apple-macosx15.0.0"

define void @f(ptr %a, ptr %b, ptr %c, ptr %d, ptr %x, ptr %y, i64 %n) "target-cpu"="apple-m4" {
entry:
  %g = icmp sgt i64 %n, 0
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %pd = getelementptr inbounds i32, ptr %d, i64 %i
  %va = load i32, ptr %pa
  %vb = load i32, ptr %pb
  %vc = load i32, ptr %pc
  %vd = load i32, ptr %pd
  %db = or i32 %vb, 1
  %dd = or i32 %vd, 1
  %q0 = udiv i32 %va, %db
  %q1 = udiv i32 %vc, %dd
  %px = getelementptr inbounds i32, ptr %x, i64 %i
  %py = getelementptr inbounds i32, ptr %y, i64 %i
  store i32 %q0, ptr %px
  store i32 %q1, ptr %py
  %i.next = add nuw nsw i64 %i, 1
  %cmp = icmp slt i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
