; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -O0 < %s | FileCheck %s

; Lane-0 unsigned divide through double precision, no integer divide.
declare <4 x i32> @llvm.savio.sv.udiv.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32>, <4 x i32>)

; CHECK-LABEL: f_udiv:
; CHECK-NOT: divl
; CHECK: vdivsd
; CHECK-NOT: divl
define <4 x i32> @f_udiv(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.udiv.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_urem:
; CHECK-NOT: divl
; CHECK: vdivsd
; CHECK-NOT: divl
define <4 x i32> @f_urem(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}
