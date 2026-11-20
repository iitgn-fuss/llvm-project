; RUN: opt -passes=savio-ir,verify -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -S %s | FileCheck %s

; Verifies that vector select and vector intrinsics (smin) are scalarized into
; per-lane scalar operations and scalar loads/stores.

target triple = "arm64-apple-macos"

; CHECK-LABEL: @test_select(
; CHECK: extractelement <4 x i1> %cond, i64 0
; CHECK: load i32, ptr %pk
; CHECK: select i1 {{.*}}, i32 {{.*}}, i32 42
; CHECK: store i32 {{.*}}, ptr %po
define void @test_select(ptr noalias %k, ptr noalias %o, <4 x i1> %cond, i64 %n.vec) {
entry:
  br label %vector.body

vector.body:
  %index = phi i64 [ 0, %entry ], [ %index.next, %vector.body ]
  %pk = getelementptr inbounds nuw i32, ptr %k, i64 %index
  %w = load <4 x i32>, ptr %pk, align 16
  %sel = select <4 x i1> %cond, <4 x i32> %w, <4 x i32> splat (i32 42)
  %po = getelementptr inbounds nuw i32, ptr %o, i64 %index
  store <4 x i32> %sel, ptr %po, align 16
  %index.next = add nuw i64 %index, 4
  %done = icmp eq i64 %index.next, %n.vec
  br i1 %done, label %exit, label %vector.body

exit:
  ret void
}

; CHECK-LABEL: @test_smin(
; CHECK: load i32, ptr %pk
; CHECK: call i32 @llvm.smin.i32(i32 {{.*}}, i32 100)
; CHECK: store i32 {{.*}}, ptr %po
define void @test_smin(ptr noalias %k, ptr noalias %o, i64 %n.vec) {
entry:
  br label %vector.body

vector.body:
  %index = phi i64 [ 0, %entry ], [ %index.next, %vector.body ]
  %pk = getelementptr inbounds nuw i32, ptr %k, i64 %index
  %w = load <4 x i32>, ptr %pk, align 16
  %min = call <4 x i32> @llvm.smin.v4i32(<4 x i32> %w, <4 x i32> splat (i32 100))
  %po = getelementptr inbounds nuw i32, ptr %o, i64 %index
  store <4 x i32> %min, ptr %po, align 16
  %index.next = add nuw i64 %index, 4
  %done = icmp eq i64 %index.next, %n.vec
  br i1 %done, label %exit, label %vector.body

exit:
  ret void
}
