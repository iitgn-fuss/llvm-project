; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s
; RUN: llc -enable-new-pm -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids < %s | FileCheck %s

; Four lanes of an <8 x i32> through one ymm double divide.
declare <8 x i32> @llvm.savio.fpdiv4.udiv.v8i32(<8 x i32>, <8 x i32>)

; CHECK-LABEL: f4:
; CHECK-NOT: divl
; CHECK: vdivpd {{.*}}%ymm
; CHECK-NOT: vdivpd
; CHECK-NOT: divl
define <8 x i32> @f4(<8 x i32> %a, <8 x i32> %b) {
  %r = call <8 x i32> @llvm.savio.fpdiv4.udiv.v8i32(<8 x i32> %a, <8 x i32> %b)
  ret <8 x i32> %r
}
