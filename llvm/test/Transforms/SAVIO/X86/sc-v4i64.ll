; RUN: opt -passes=savio-ir,verify -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s
; A <4 x i64> vector (the splitmix64 body, two copies): forcing one instance Scalarizes the first copy into four
; 64-bit scalar lanes (scalar loads at lane offsets 0, 8, 16, 24); the second copy stays a vector.
; CHECK-LABEL: @f(
; CHECK-COUNT-4: load i64, ptr
; CHECK-NOT: load <4 x i64>, ptr %pk0
; CHECK: mul i64
; CHECK-COUNT-4: store i64
; CHECK: load <4 x i64>, ptr %pk1
; CHECK: store <4 x i64> %f4_1
; CHECK-NOT: load i64
target triple = "x86_64-unknown-linux-gnu"

define void @f(ptr noalias %k, ptr noalias %o, i64 %n.vec) {
entry:
  br label %vector.body

vector.body:
  %index = phi i64 [ 0, %entry ], [ %index.next, %vector.body ]
  %pk0 = getelementptr inbounds nuw i64, ptr %k, i64 %index
  %w0 = load <4 x i64>, ptr %pk0, align 32, !tbaa !0
  %s0_0 = lshr <4 x i64> %w0, splat (i64 30)
  %f0_0 = xor <4 x i64> %s0_0, %w0
  %f1_0 = mul <4 x i64> %f0_0, splat (i64 -4658895280553007687)
  %s2_0 = lshr <4 x i64> %f1_0, splat (i64 27)
  %f2_0 = xor <4 x i64> %s2_0, %f1_0
  %f3_0 = mul <4 x i64> %f2_0, splat (i64 -7723592293110705685)
  %s4_0 = lshr <4 x i64> %f3_0, splat (i64 31)
  %f4_0 = xor <4 x i64> %s4_0, %f3_0
  %po0 = getelementptr inbounds nuw i64, ptr %o, i64 %index
  store <4 x i64> %f4_0, ptr %po0, align 32, !tbaa !0
  %index1 = add nuw i64 %index, 4
  %pk1 = getelementptr inbounds nuw i64, ptr %k, i64 %index1
  %w1 = load <4 x i64>, ptr %pk1, align 32, !tbaa !0
  %s0_1 = lshr <4 x i64> %w1, splat (i64 30)
  %f0_1 = xor <4 x i64> %s0_1, %w1
  %f1_1 = mul <4 x i64> %f0_1, splat (i64 -4658895280553007687)
  %s2_1 = lshr <4 x i64> %f1_1, splat (i64 27)
  %f2_1 = xor <4 x i64> %s2_1, %f1_1
  %f3_1 = mul <4 x i64> %f2_1, splat (i64 -7723592293110705685)
  %s4_1 = lshr <4 x i64> %f3_1, splat (i64 31)
  %f4_1 = xor <4 x i64> %s4_1, %f3_1
  %po1 = getelementptr inbounds nuw i64, ptr %o, i64 %index1
  store <4 x i64> %f4_1, ptr %po1, align 32, !tbaa !0
  %index.next = add nuw i64 %index, 8
  %done = icmp eq i64 %index.next, %n.vec
  br i1 %done, label %exit, label %vector.body

exit:
  ret void
}

!0 = !{!1, !1, i64 0}
!1 = !{!"int", !2, i64 0}
!2 = !{!"omnipotent char", !3, i64 0}
!3 = !{!"Simple C/C++ TBAA"}
