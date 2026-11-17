; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -mattr=-prefer-256-bit < %s | FileCheck %s

; Eight lanes through one zmm double divide.
declare <8 x i32> @llvm.savio.fpdiv8.udiv.v8i32(<8 x i32>, <8 x i32>)

; CHECK-LABEL: f8:
; CHECK-NOT: divl
; CHECK: vdivpd {{.*}}%zmm
; CHECK-NOT: vdivpd
; CHECK-NOT: divl
define <8 x i32> @f8(<8 x i32> %a, <8 x i32> %b) #0 {
  %r = call <8 x i32> @llvm.savio.fpdiv8.udiv.v8i32(<8 x i32> %a, <8 x i32> %b)
  ret <8 x i32> %r
}
attributes #0 = { "prefer-vector-width"="512" }
