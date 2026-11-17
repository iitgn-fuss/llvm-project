; RUN: opt -passes=savio,verify -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; Unrolling and Scalarize together, with no force flag. On the test3scv fixture (two scalar units, one vector unit) a
; loop of twelve <4 x i32> operations on one vector load is bound by the vector unit; the cost search unrolls it by
; three and Scalarizes one of the three copies (T 10.00 -> 6.67 per original iteration). The pass unrolls, forms the
; regions of the unrolled loop again, decides again and applies the decision: the first copy becomes four scalar loads,
; per-lane operations and four scalar stores, the other two copies stay vectors, and the remainder loop is the
; original vector body.
; CHECK: remark: {{.*}}: T 10.00 -> 6.67 (resource), 1/3 of slice {{[0-9]+}}, unrolled by 3 (estimated 1/3 T 8.00, applied 1/3 T 6.67)
; CHECK: loop:
; CHECK-COUNT-4: load i32
; CHECK-NOT: load
; CHECK: add i32
; CHECK-COUNT-4: store i32
; CHECK: load <4 x i32>
; CHECK: store <4 x i32>
; CHECK: load <4 x i32>
; CHECK: store <4 x i32>
; CHECK: br i1 %niter.ncmp.2, label %loop
; CHECK: loop.epil:
; CHECK: load <4 x i32>
; CHECK-NOT: load i32
target triple = "arm64-apple-macos"

define void @f(ptr noalias %a, ptr noalias %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds <4 x i32>, ptr %a, i64 %i
  %x = load <4 x i32>, ptr %pa, align 4
  %t0 = add <4 x i32> %x, splat (i32 3)
  %t1 = xor <4 x i32> %t0, splat (i32 5)
  %t2 = mul <4 x i32> %t1, splat (i32 7)
  %t3 = add <4 x i32> %t2, splat (i32 9)
  %t4 = add <4 x i32> %t3, splat (i32 3)
  %t5 = xor <4 x i32> %t4, splat (i32 5)
  %t6 = mul <4 x i32> %t5, splat (i32 7)
  %t7 = add <4 x i32> %t6, splat (i32 9)
  %t8 = add <4 x i32> %t7, splat (i32 3)
  %t9 = xor <4 x i32> %t8, splat (i32 5)
  %t10 = mul <4 x i32> %t9, splat (i32 7)
  %t11 = add <4 x i32> %t10, splat (i32 9)
  %pb = getelementptr inbounds <4 x i32>, ptr %b, i64 %i
  store <4 x i32> %t11, ptr %pb, align 4
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
