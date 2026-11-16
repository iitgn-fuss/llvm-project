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

; All four lanes: lanes {0,1} and {2,3} (the second pair moved to the low lanes
; first), recombined. No integer divide remains.
; CHECK-LABEL: f_urem4:
; CHECK-NOT: udiv w
; CHECK: fdiv.2d
; CHECK: fdiv.2d
; CHECK-NOT: udiv w
define <4 x i32> @f_urem4(<4 x i32> %a, <4 x i32> %b) {
  %lo = call <4 x i32> @llvm.savio.fpdiv2.urem.v4i32(<4 x i32> %a, <4 x i32> %b)
  %a2 = shufflevector <4 x i32> %a, <4 x i32> poison, <4 x i32> <i32 2, i32 3, i32 poison, i32 poison>
  %b2 = shufflevector <4 x i32> %b, <4 x i32> poison, <4 x i32> <i32 2, i32 3, i32 poison, i32 poison>
  %hi = call <4 x i32> @llvm.savio.fpdiv2.urem.v4i32(<4 x i32> %a2, <4 x i32> %b2)
  %r = shufflevector <4 x i32> %lo, <4 x i32> %hi, <4 x i32> <i32 0, i32 1, i32 4, i32 5>
  ret <4 x i32> %r
}
