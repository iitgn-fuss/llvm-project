; RUN: opt -passes=savio-ir,verify -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -pass-remarks=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=REM
; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s --check-prefix=NOKEY
; A 256-bit <8 x i32> vector (the fmix32 body of ../sc-rewrite.ll, two copies): forcing one instance Scalarizes the
; first copy into eight scalar lanes (scalar loads at lane offsets 0..28); the second copy stays a vector. Nothing is
; hard-wired to four lanes: the keys are v8i32 / i32. A port-info without the v8i32 keys (test2s1v) changes nothing.
; CHECK-LABEL: @f(
; CHECK-COUNT-8: load i32, ptr
; CHECK-NOT: load <8 x i32>, ptr %pk0
; CHECK: mul i32
; CHECK-COUNT-8: store i32
; The second copy is untouched.
; CHECK: load <8 x i32>, ptr %pk1
; CHECK: store <8 x i32> %f4_1
; CHECK-NOT: load i32
; REM: remark: {{.*}}1/2 of slice
; NOKEY-NOT: load i32
; NOKEY: load <8 x i32>, ptr %pk0
target triple = "x86_64-unknown-linux-gnu"

define void @f(ptr noalias %k, ptr noalias %o, i64 %n.vec) {
entry:
  br label %vector.body

vector.body:
  %index = phi i64 [ 0, %entry ], [ %index.next, %vector.body ]
  %pk0 = getelementptr inbounds nuw i32, ptr %k, i64 %index
  %w0 = load <8 x i32>, ptr %pk0, align 32, !tbaa !0
  %s0_0 = lshr <8 x i32> %w0, splat (i32 16)
  %f0_0 = xor <8 x i32> %s0_0, %w0
  %f1_0 = mul <8 x i32> %f0_0, splat (i32 -2048144789)
  %s2_0 = lshr <8 x i32> %f1_0, splat (i32 13)
  %f2_0 = xor <8 x i32> %s2_0, %f1_0
  %f3_0 = mul <8 x i32> %f2_0, splat (i32 -1028477387)
  %s4_0 = lshr <8 x i32> %f3_0, splat (i32 16)
  %f4_0 = xor <8 x i32> %s4_0, %f3_0
  %po0 = getelementptr inbounds nuw i32, ptr %o, i64 %index
  store <8 x i32> %f4_0, ptr %po0, align 32, !tbaa !0
  %index1 = add nuw i64 %index, 8
  %pk1 = getelementptr inbounds nuw i32, ptr %k, i64 %index1
  %w1 = load <8 x i32>, ptr %pk1, align 32, !tbaa !0
  %s0_1 = lshr <8 x i32> %w1, splat (i32 16)
  %f0_1 = xor <8 x i32> %s0_1, %w1
  %f1_1 = mul <8 x i32> %f0_1, splat (i32 -2048144789)
  %s2_1 = lshr <8 x i32> %f1_1, splat (i32 13)
  %f2_1 = xor <8 x i32> %s2_1, %f1_1
  %f3_1 = mul <8 x i32> %f2_1, splat (i32 -1028477387)
  %s4_1 = lshr <8 x i32> %f3_1, splat (i32 16)
  %f4_1 = xor <8 x i32> %s4_1, %f3_1
  %po1 = getelementptr inbounds nuw i32, ptr %o, i64 %index1
  store <8 x i32> %f4_1, ptr %po1, align 32, !tbaa !0
  %index.next = add nuw i64 %index, 16
  %done = icmp eq i64 %index.next, %n.vec
  br i1 %done, label %exit, label %vector.body

exit:
  ret void
}

!0 = !{!1, !1, i64 0}
!1 = !{!"int", !2, i64 0}
!2 = !{!"omnipotent char", !3, i64 0}
!3 = !{!"Simple C/C++ TBAA"}
