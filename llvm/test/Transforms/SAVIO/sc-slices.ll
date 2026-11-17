; REQUIRES: asserts
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -S %s | FileCheck %s --check-prefix=IR
; The -O3 vector body of fmix32 (bench/savio-pass/isolation/scalarize/k.c) with four interleaved copies, as the loop
; vectorizer leaves it at interleave count 4: each copy is a <4 x i32> load, eight vector operations, a <4 x i32> store.
; Every copy is one Scalarize slice, all four are instances of one class (same operations, constants, owned load and
; store). The class is not Singleton Vectorize's; the scalar index arithmetic forms no group.
; CHECK: savio: group of 8 ops (scalarize slice, class [[C:[0-9]+]]) in %vector.body
; CHECK-NEXT: load: %w0
; CHECK-NEXT: liveout: %h0 (stored)
; CHECK-NEXT: store: %po0
; CHECK: savio: group of 8 ops (scalarize slice, class [[C]]) in %vector.body
; CHECK-NEXT: load: %w1
; CHECK-NEXT: liveout: %h1 (stored)
; CHECK-NEXT: store: %po1
; CHECK: savio: group of 8 ops (scalarize slice, class [[C]]) in %vector.body
; CHECK-NEXT: load: %w2
; CHECK-NEXT: liveout: %h2 (stored)
; CHECK-NEXT: store: %po2
; CHECK: savio: group of 8 ops (scalarize slice, class [[C]]) in %vector.body
; CHECK-NEXT: load: %w3
; CHECK-NEXT: liveout: %h3 (stored)
; CHECK-NEXT: store: %po3
; CHECK-NOT: savio: group
; All four copies are Scalarized: 16 scalar loads and stores, no vector operation or memory access left.
; IR-COUNT-16: load i32
; IR-NOT: <4 x i32>
; IR-NOT: @llvm.savio
target triple = "arm64-apple-macos"

define void @o3(ptr noalias %k, ptr noalias %o, i64 %n.vec) {
entry:
  br label %vector.body

vector.body:
  %index = phi i64 [ 0, %entry ], [ %index.next, %vector.body ]
  %pk0 = getelementptr inbounds nuw i32, ptr %k, i64 %index
  %w0 = load <4 x i32>, ptr %pk0, align 4
  %a0 = lshr <4 x i32> %w0, splat (i32 16)
  %b0 = xor <4 x i32> %a0, %w0
  %c0 = mul <4 x i32> %b0, splat (i32 -2048144789)
  %d0 = lshr <4 x i32> %c0, splat (i32 13)
  %e0 = xor <4 x i32> %d0, %c0
  %f0 = mul <4 x i32> %e0, splat (i32 -1028477387)
  %g0 = lshr <4 x i32> %f0, splat (i32 16)
  %h0 = xor <4 x i32> %g0, %f0
  %po0 = getelementptr inbounds nuw i32, ptr %o, i64 %index
  store <4 x i32> %h0, ptr %po0, align 4
  %index1 = add nuw i64 %index, 4
  %pk1 = getelementptr inbounds nuw i32, ptr %k, i64 %index1
  %w1 = load <4 x i32>, ptr %pk1, align 4
  %a1 = lshr <4 x i32> %w1, splat (i32 16)
  %b1 = xor <4 x i32> %a1, %w1
  %c1 = mul <4 x i32> %b1, splat (i32 -2048144789)
  %d1 = lshr <4 x i32> %c1, splat (i32 13)
  %e1 = xor <4 x i32> %d1, %c1
  %f1 = mul <4 x i32> %e1, splat (i32 -1028477387)
  %g1 = lshr <4 x i32> %f1, splat (i32 16)
  %h1 = xor <4 x i32> %g1, %f1
  %po1 = getelementptr inbounds nuw i32, ptr %o, i64 %index1
  store <4 x i32> %h1, ptr %po1, align 4
  %index2 = add nuw i64 %index, 8
  %pk2 = getelementptr inbounds nuw i32, ptr %k, i64 %index2
  %w2 = load <4 x i32>, ptr %pk2, align 4
  %a2 = lshr <4 x i32> %w2, splat (i32 16)
  %b2 = xor <4 x i32> %a2, %w2
  %c2 = mul <4 x i32> %b2, splat (i32 -2048144789)
  %d2 = lshr <4 x i32> %c2, splat (i32 13)
  %e2 = xor <4 x i32> %d2, %c2
  %f2 = mul <4 x i32> %e2, splat (i32 -1028477387)
  %g2 = lshr <4 x i32> %f2, splat (i32 16)
  %h2 = xor <4 x i32> %g2, %f2
  %po2 = getelementptr inbounds nuw i32, ptr %o, i64 %index2
  store <4 x i32> %h2, ptr %po2, align 4
  %index3 = add nuw i64 %index, 12
  %pk3 = getelementptr inbounds nuw i32, ptr %k, i64 %index3
  %w3 = load <4 x i32>, ptr %pk3, align 4
  %a3 = lshr <4 x i32> %w3, splat (i32 16)
  %b3 = xor <4 x i32> %a3, %w3
  %c3 = mul <4 x i32> %b3, splat (i32 -2048144789)
  %d3 = lshr <4 x i32> %c3, splat (i32 13)
  %e3 = xor <4 x i32> %d3, %c3
  %f3 = mul <4 x i32> %e3, splat (i32 -1028477387)
  %g3 = lshr <4 x i32> %f3, splat (i32 16)
  %h3 = xor <4 x i32> %g3, %f3
  %po3 = getelementptr inbounds nuw i32, ptr %o, i64 %index3
  store <4 x i32> %h3, ptr %po3, align 4
  %index.next = add nuw i64 %index, 16
  %done = icmp eq i64 %index.next, %n.vec
  br i1 %done, label %exit, label %vector.body

exit:
  ret void
}
