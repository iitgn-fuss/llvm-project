; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -O0 < %s | FileCheck %s
; RUN: llc -enable-new-pm -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s

; The lane-0 add stays in the vector unit.
declare <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32>, <4 x i32>)

; CHECK-LABEL: f_add:
; CHECK: vpaddd {{.*}}%xmm
; CHECK-NOT: addl
; CHECK-NOT: SAVIO
define <4 x i32> @f_add(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}
