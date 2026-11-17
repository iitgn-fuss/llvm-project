; RUN: opt -passes=savio-ir -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -S %s | FileCheck %s
; RUN: opt -passes=savio-ir,verify -savio-force-count=1 -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -disable-output %s
; Forcing one instance of the four-copy fmix32 loop Scalarizes the first copy: four scalar loads at lane offsets 0, 4,
; 8, 12 with alignment gcd(16, offset) (the vector load is align 16 here), the per-lane operation chains and four
; scalar stores; the other three copies stay vectors.
; CHECK-LABEL: @o3(
; CHECK: %pk0 = getelementptr
; CHECK: load i32, ptr %pk0, align 16, !tbaa [[T:![0-9]+]]
; CHECK: [[P1:%.*]] = getelementptr i8, ptr %pk0, i64 4
; CHECK: load i32, ptr [[P1]], align 4, !tbaa [[T]]
; CHECK: [[P2:%.*]] = getelementptr i8, ptr %pk0, i64 8
; CHECK: load i32, ptr [[P2]], align 8, !tbaa [[T]]
; CHECK: [[P3:%.*]] = getelementptr i8, ptr %pk0, i64 12
; CHECK: load i32, ptr [[P3]], align 4, !tbaa [[T]]
; CHECK: lshr i32
; CHECK: mul i32
; CHECK: store i32 {{.*}}, ptr %po0, align 16, !tbaa [[T]]
; CHECK: store i32 {{.*}}, align 4, !tbaa [[T]]
; CHECK: store i32 {{.*}}, align 8, !tbaa [[T]]
; CHECK: store i32 {{.*}}, align 4, !tbaa [[T]]
; The other copies are untouched.
; CHECK: load <4 x i32>, ptr %pk1, align 16, !tbaa [[T]]
; CHECK: store <4 x i32> %h1
; CHECK: load <4 x i32>, ptr %pk2
; CHECK: store <4 x i32> %h2
; CHECK: load <4 x i32>, ptr %pk3
; CHECK: store <4 x i32> %h3
; CHECK-NOT: load i32
target triple = "arm64-apple-macos"

define void @o3(ptr noalias %k, ptr noalias %o, i64 %n.vec) {
entry:
  br label %vector.body

vector.body:
  %index = phi i64 [ 0, %entry ], [ %index.next, %vector.body ]
  %pk0 = getelementptr inbounds nuw i32, ptr %k, i64 %index
  %w0 = load <4 x i32>, ptr %pk0, align 16, !tbaa !0
  %a0 = lshr <4 x i32> %w0, splat (i32 16)
  %b0 = xor <4 x i32> %a0, %w0
  %c0 = mul <4 x i32> %b0, splat (i32 -2048144789)
  %d0 = lshr <4 x i32> %c0, splat (i32 13)
  %e0 = xor <4 x i32> %d0, %c0
  %f0 = mul <4 x i32> %e0, splat (i32 -1028477387)
  %g0 = lshr <4 x i32> %f0, splat (i32 16)
  %h0 = xor <4 x i32> %g0, %f0
  %po0 = getelementptr inbounds nuw i32, ptr %o, i64 %index
  store <4 x i32> %h0, ptr %po0, align 16, !tbaa !0
  %index1 = add nuw i64 %index, 4
  %pk1 = getelementptr inbounds nuw i32, ptr %k, i64 %index1
  %w1 = load <4 x i32>, ptr %pk1, align 16, !tbaa !0
  %a1 = lshr <4 x i32> %w1, splat (i32 16)
  %b1 = xor <4 x i32> %a1, %w1
  %c1 = mul <4 x i32> %b1, splat (i32 -2048144789)
  %d1 = lshr <4 x i32> %c1, splat (i32 13)
  %e1 = xor <4 x i32> %d1, %c1
  %f1 = mul <4 x i32> %e1, splat (i32 -1028477387)
  %g1 = lshr <4 x i32> %f1, splat (i32 16)
  %h1 = xor <4 x i32> %g1, %f1
  %po1 = getelementptr inbounds nuw i32, ptr %o, i64 %index1
  store <4 x i32> %h1, ptr %po1, align 16, !tbaa !0
  %index2 = add nuw i64 %index, 8
  %pk2 = getelementptr inbounds nuw i32, ptr %k, i64 %index2
  %w2 = load <4 x i32>, ptr %pk2, align 16, !tbaa !0
  %a2 = lshr <4 x i32> %w2, splat (i32 16)
  %b2 = xor <4 x i32> %a2, %w2
  %c2 = mul <4 x i32> %b2, splat (i32 -2048144789)
  %d2 = lshr <4 x i32> %c2, splat (i32 13)
  %e2 = xor <4 x i32> %d2, %c2
  %f2 = mul <4 x i32> %e2, splat (i32 -1028477387)
  %g2 = lshr <4 x i32> %f2, splat (i32 16)
  %h2 = xor <4 x i32> %g2, %f2
  %po2 = getelementptr inbounds nuw i32, ptr %o, i64 %index2
  store <4 x i32> %h2, ptr %po2, align 16, !tbaa !0
  %index3 = add nuw i64 %index, 12
  %pk3 = getelementptr inbounds nuw i32, ptr %k, i64 %index3
  %w3 = load <4 x i32>, ptr %pk3, align 16, !tbaa !0
  %a3 = lshr <4 x i32> %w3, splat (i32 16)
  %b3 = xor <4 x i32> %a3, %w3
  %c3 = mul <4 x i32> %b3, splat (i32 -2048144789)
  %d3 = lshr <4 x i32> %c3, splat (i32 13)
  %e3 = xor <4 x i32> %d3, %c3
  %f3 = mul <4 x i32> %e3, splat (i32 -1028477387)
  %g3 = lshr <4 x i32> %f3, splat (i32 16)
  %h3 = xor <4 x i32> %g3, %f3
  %po3 = getelementptr inbounds nuw i32, ptr %o, i64 %index3
  store <4 x i32> %h3, ptr %po3, align 16, !tbaa !0
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
