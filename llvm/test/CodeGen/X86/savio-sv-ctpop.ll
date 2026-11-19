; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -O0 < %s | FileCheck %s
; RUN: llc -enable-new-pm -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s

; The lane-0 ctpop stays in the vector unit (one operand; lane 0 of the result is
; the count of lane 0, the other lanes are poison).
declare <4 x i32> @llvm.savio.sv.ctpop.v4i32(<4 x i32>)

; CHECK-LABEL: f_ctpop:
; CHECK: vpopcntd {{.*}}%xmm
; CHECK-NOT: popcntl
; CHECK-NOT: SAVIO
define <4 x i32> @f_ctpop(<4 x i32> %a) {
  %r = call <4 x i32> @llvm.savio.sv.ctpop.v4i32(<4 x i32> %a)
  ret <4 x i32> %r
}
