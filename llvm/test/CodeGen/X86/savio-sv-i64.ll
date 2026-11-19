; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s
; RUN: llc -enable-new-pm -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s

declare <2 x i64> @llvm.savio.sv.add.v2i64(<2 x i64>, <2 x i64>)
declare <2 x i64> @llvm.savio.sv.sub.v2i64(<2 x i64>, <2 x i64>)
declare <2 x i64> @llvm.savio.sv.mul.v2i64(<2 x i64>, <2 x i64>)

; CHECK-LABEL: test_sv_add_i64:
; CHECK: vpaddq
define <2 x i64> @test_sv_add_i64(<2 x i64> %a, <2 x i64> %b) {
  %r = call <2 x i64> @llvm.savio.sv.add.v2i64(<2 x i64> %a, <2 x i64> %b)
  ret <2 x i64> %r
}

; CHECK-LABEL: test_sv_sub_i64:
; CHECK: vpsubq
define <2 x i64> @test_sv_sub_i64(<2 x i64> %a, <2 x i64> %b) {
  %r = call <2 x i64> @llvm.savio.sv.sub.v2i64(<2 x i64> %a, <2 x i64> %b)
  ret <2 x i64> %r
}

; CHECK-LABEL: test_sv_mul_i64:
; CHECK: vpmullq
define <2 x i64> @test_sv_mul_i64(<2 x i64> %a, <2 x i64> %b) {
  %r = call <2 x i64> @llvm.savio.sv.mul.v2i64(<2 x i64> %a, <2 x i64> %b)
  ret <2 x i64> %r
}
