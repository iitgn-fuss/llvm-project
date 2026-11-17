; RUN: not opt -passes=verify -disable-output %s 2>&1 | FileCheck %s
; fpdiv<L> needs L lanes: fpdiv8 on <4 x i32> is rejected, as is a non-i32 element type.
; CHECK: llvm.savio.fpdiv lane count exceeds vector width
; CHECK: llvm.savio.fpdiv element type must be i32
define <4 x i32> @f(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.fpdiv8.udiv.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

define <4 x i64> @g(<4 x i64> %a, <4 x i64> %b) {
  %r = call <4 x i64> @llvm.savio.fpdiv2.udiv.v4i64(<4 x i64> %a, <4 x i64> %b)
  ret <4 x i64> %r
}

declare <4 x i32> @llvm.savio.fpdiv8.udiv.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i64> @llvm.savio.fpdiv2.udiv.v4i64(<4 x i64>, <4 x i64>)
