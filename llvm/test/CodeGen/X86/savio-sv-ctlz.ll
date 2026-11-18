; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -O0 < %s | FileCheck %s
; RUN: llc -enable-new-pm -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s

; savio-expand lowers lane 0 to a scalar ctlz intrinsic.  Other lanes stay
; poison and are not materialized as vector count instructions.
declare <4 x i32> @llvm.savio.sv.ctlz.v4i32(<4 x i32>)

; CHECK-LABEL: f_ctlz:
; CHECK: vmovd %xmm0, %eax
; CHECK: lzcntl %eax, %eax
; CHECK: vmovd %eax, %xmm0
; CHECK-NOT: vplzcntd
; CHECK-NOT: SAVIO
define <4 x i32> @f_ctlz(<4 x i32> %a) {
  %r = call <4 x i32> @llvm.savio.sv.ctlz.v4i32(<4 x i32> %a)
  ret <4 x i32> %r
}
