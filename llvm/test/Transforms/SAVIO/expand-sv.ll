; RUN: opt -passes=savio-expand -S %s | FileCheck %s
; RUN: opt -passes=savio-expand,instsimplify -S %s | FileCheck %s --check-prefix=CONST

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

; One-operand counts: lane 0 only, with poison in every other lane.
; ctlz(0) is the element width; is_zero_poison is false.
declare <4 x i32> @llvm.savio.sv.ctpop.v4i32(<4 x i32>)
declare <4 x i32> @llvm.savio.sv.ctlz.v4i32(<4 x i32>)

; CHECK-LABEL: @ctpop(
; CHECK: extractelement <4 x i32> %a, i64 0
; CHECK: call i32 @llvm.ctpop.i32(i32 %{{.*}})
; CHECK: insertelement <4 x i32> poison, i32 %{{.*}}, i64 0
; CHECK-NOT: llvm.savio
define <4 x i32> @ctpop(<4 x i32> %a) {
  %r = call <4 x i32> @llvm.savio.sv.ctpop.v4i32(<4 x i32> %a)
  ret <4 x i32> %r
}

; CHECK-LABEL: @ctlz(
; CHECK: extractelement <4 x i32> %a, i64 0
; CHECK: call i32 @llvm.ctlz.i32(i32 %{{.*}}, i1 false)
; CHECK: insertelement <4 x i32> poison, i32 %{{.*}}, i64 0
; CHECK-NOT: llvm.savio
define <4 x i32> @ctlz(<4 x i32> %a) {
  %r = call <4 x i32> @llvm.savio.sv.ctlz.v4i32(<4 x i32> %a)
  ret <4 x i32> %r
}

; A constant vector through the expansion and constant folding. Only lane 0 is
; what the intrinsic defines (ctlz of 0 is 32, ctpop of 0 is 0); every other
; lane is poison in the intrinsic contract.
; CONST-LABEL: @ctlz_const(
; CONST: ret <4 x i32> <i32 32, i32 poison, i32 poison, i32 poison>
define <4 x i32> @ctlz_const() {
  %r = call <4 x i32> @llvm.savio.sv.ctlz.v4i32(<4 x i32> <i32 0, i32 -1, i32 1, i32 0>)
  ret <4 x i32> %r
}

; CONST-LABEL: @ctpop_const(
; CONST: ret <4 x i32> <i32 0, i32 poison, i32 poison, i32 poison>
define <4 x i32> @ctpop_const() {
  %r = call <4 x i32> @llvm.savio.sv.ctpop.v4i32(<4 x i32> <i32 0, i32 -1, i32 1, i32 0>)
  ret <4 x i32> %r
}
