; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 < %s | FileCheck %s
; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 -O0 < %s | FileCheck %s

; Lanes 0 and 1 of an unsigned divide through double precision, two at a time:
; widen both operands (ushll .2d), convert (ucvtf .2d), fdiv .2d, fcvtzu .2d,
; narrow (xtn). urem adds an mls.2s. Lanes 2 and 3 of the result are poison.

declare <4 x i32> @llvm.savio.fpdiv2.udiv.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.fpdiv2.urem.v4i32(<4 x i32>, <4 x i32>)

; CHECK-LABEL: f_udiv:
; CHECK-NOT: udiv w
; CHECK: ushll.2d
; CHECK: ucvtf.2d
; CHECK: ushll.2d
; CHECK: ucvtf.2d
; CHECK: fdiv.2d
; CHECK: fcvtzu.2d
; CHECK: xtn.2s
; CHECK-NOT: mls.2s
; CHECK-NOT: SAVIO_
define <4 x i32> @f_udiv(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.fpdiv2.udiv.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_urem:
; CHECK-NOT: udiv w
; CHECK: ushll.2d
; CHECK: ucvtf.2d
; CHECK: ushll.2d
; CHECK: ucvtf.2d
; CHECK: fdiv.2d
; CHECK: fcvtzu.2d
; CHECK: xtn.2s
; CHECK: mls.2s
; CHECK-NOT: SAVIO_
define <4 x i32> @f_urem(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.fpdiv2.urem.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

declare <4 x i32> @llvm.savio.fpdiv4.udiv.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.fpdiv4.urem.v4i32(<4 x i32>, <4 x i32>)

; All four lanes: the high pair widens with ushll2 (no lane move), two fdiv.2d,
; one uzp1 narrows both quotients, urem adds one mls.4s. No integer divide.
; CHECK-LABEL: f_urem4:
; CHECK-NOT: udiv w
; CHECK: fdiv.2d
; CHECK: fdiv.2d
; CHECK: uzp1.4s
; CHECK: mls.4s
; CHECK-NOT: udiv w
; CHECK-NOT: SAVIO_
define <4 x i32> @f_urem4(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.fpdiv4.urem.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_udiv4:
; CHECK: ushll2.2d
; CHECK: fdiv.2d
; CHECK: uzp1.4s
; CHECK-NOT: mls
; CHECK-NOT: udiv w
define <4 x i32> @f_udiv4(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.fpdiv4.udiv.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}
