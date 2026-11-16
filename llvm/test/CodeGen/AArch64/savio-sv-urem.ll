; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 < %s | FileCheck %s
; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 -O0 < %s | FileCheck %s

; Lane-0 unsigned divide through double precision: widen and convert each
; operand, fdiv, fcvtzu. urem adds an mls.

declare <4 x i32> @llvm.savio.sv.udiv.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32>, <4 x i32>)

; CHECK-LABEL: f_udiv:
; CHECK-NOT: udiv w
; CHECK: ushll.2d
; CHECK: ucvtf d
; CHECK: ushll.2d
; CHECK: ucvtf d
; CHECK: fdiv d
; CHECK: fcvtzu d
; CHECK-NOT: mls.2s
; CHECK-NOT: SAVIO_
define <4 x i32> @f_udiv(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.udiv.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_urem:
; CHECK-NOT: udiv w
; CHECK: ushll.2d
; CHECK: ucvtf d
; CHECK: ushll.2d
; CHECK: ucvtf d
; CHECK: fdiv d
; CHECK: fcvtzu d
; CHECK: mls.2s
; CHECK-NOT: udiv w
; CHECK-NOT: msub
; CHECK-NOT: SAVIO_
define <4 x i32> @f_urem(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}
