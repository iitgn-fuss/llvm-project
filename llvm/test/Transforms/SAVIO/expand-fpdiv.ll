; RUN: opt -passes=savio-expand -S %s | FileCheck %s

declare <4 x i32> @llvm.savio.fpdiv2.udiv.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.fpdiv4.urem.v4i32(<4 x i32>, <4 x i32>)
declare <8 x i32> @llvm.savio.fpdiv4.udiv.v8i32(<8 x i32>, <8 x i32>)
declare <8 x i32> @llvm.savio.fpdiv8.urem.v8i32(<8 x i32>, <8 x i32>)

; L < lanes: shuffle the low lanes, double divide, widen with poison.
; CHECK-LABEL: @d2(
; CHECK: shufflevector <4 x i32> %a, <4 x i32> poison, <2 x i32> <i32 0, i32 1>
; CHECK: shufflevector <4 x i32> %b, <4 x i32> poison, <2 x i32> <i32 0, i32 1>
; CHECK: uitofp <2 x i32> %{{.*}} to <2 x double>
; CHECK: uitofp <2 x i32> %{{.*}} to <2 x double>
; CHECK: fdiv <2 x double>
; CHECK: fptoui <2 x double> %{{.*}} to <2 x i32>
; CHECK: shufflevector <2 x i32> %{{.*}}, <2 x i32> poison, <4 x i32> <i32 0, i32 1, i32 poison, i32 poison>
; CHECK-NOT: udiv
define <4 x i32> @d2(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.fpdiv2.udiv.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; L == lanes: no shuffles; urem is a - q*b.
; CHECK-LABEL: @r4(
; CHECK-NOT: shufflevector
; CHECK: uitofp <4 x i32> %a to <4 x double>
; CHECK: uitofp <4 x i32> %b to <4 x double>
; CHECK: fdiv <4 x double>
; CHECK: %[[Q:.*]] = fptoui <4 x double> %{{.*}} to <4 x i32>
; CHECK: %[[M:.*]] = mul <4 x i32> %[[Q]], %b
; CHECK: sub <4 x i32> %a, %[[M]]
; CHECK-NOT: urem
define <4 x i32> @r4(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.fpdiv4.urem.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: @d4of8(
; CHECK: shufflevector <8 x i32> %a, <8 x i32> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
; CHECK: fdiv <4 x double>
; CHECK: shufflevector <4 x i32> %{{.*}}, <4 x i32> poison, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 poison, i32 poison, i32 poison, i32 poison>
define <8 x i32> @d4of8(<8 x i32> %a, <8 x i32> %b) {
  %r = call <8 x i32> @llvm.savio.fpdiv4.udiv.v8i32(<8 x i32> %a, <8 x i32> %b)
  ret <8 x i32> %r
}

; CHECK-LABEL: @r8(
; CHECK: fdiv <8 x double>
; CHECK: sub <8 x i32> %a
define <8 x i32> @r8(<8 x i32> %a, <8 x i32> %b) {
  %r = call <8 x i32> @llvm.savio.fpdiv8.urem.v8i32(<8 x i32> %a, <8 x i32> %b)
  ret <8 x i32> %r
}
