; RUN: opt -passes=savio-expand -S %s | FileCheck %s

declare <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.shl.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.udiv.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32>, <4 x i32>)

; Non-divide: the whole-vector operation (other lanes are poison anyway).
; CHECK-LABEL: @add(
; CHECK: %{{.*}} = add <4 x i32> %a, %b
; CHECK-NOT: llvm.savio
define <4 x i32> @add(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: @shl(
; CHECK: shl <4 x i32> %a, %b
define <4 x i32> @shl(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.shl.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; Divide: lane 0 through double precision, never an integer divide (the other
; lanes may be zero).
; CHECK-LABEL: @udiv(
; CHECK: extractelement <4 x i32> %a, i64 0
; CHECK: extractelement <4 x i32> %b, i64 0
; CHECK: uitofp i32 %{{.*}} to double
; CHECK: uitofp i32 %{{.*}} to double
; CHECK: fdiv double
; CHECK: fptoui double %{{.*}} to i32
; CHECK: insertelement <4 x i32> poison, i32 %{{.*}}, i64 0
; CHECK-NOT: udiv
define <4 x i32> @udiv(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.udiv.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: @urem(
; CHECK: fdiv double
; CHECK: fptoui double
; CHECK: %[[M:.*]] = mul i32
; CHECK: sub i32 %{{.*}}, %[[M]]
; CHECK: insertelement <4 x i32> poison
; CHECK-NOT: urem
define <4 x i32> @urem(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}
