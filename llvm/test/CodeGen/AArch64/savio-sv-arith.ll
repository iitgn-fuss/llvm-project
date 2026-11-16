; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 < %s | FileCheck %s
; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 -O0 -global-isel=0 < %s | FileCheck %s
; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 -O0 -global-isel=1 < %s | FileCheck %s --check-prefix=GISEL
; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 -stop-after=finalize-isel < %s | FileCheck %s --check-prefix=ISEL

; Every llvm.savio.sv.* call is selected to a SAVIO_SV_* pseudo and expanded
; before register allocation into the plain NEON instruction.

declare <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.sub.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.mul.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.and.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.or.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.xor.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.shl.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.lshr.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.ashr.v4i32(<4 x i32>, <4 x i32>)

; GlobalISel (the -O0 default) selects every operation through the same
; pseudos but has no immediate-shift pattern: a constant count is a variable one.
; GISEL-LABEL: f_add:
; GISEL: add.4s v0, v0, v1
; GISEL-LABEL: f_lshr_imm:
; GISEL: neg.4s
; GISEL: ushl.4s
; GISEL-NOT: SAVIO_

; ISEL: SAVIO_SV_ADD_V4I32
; ISEL: SAVIO_SV_SUB_V4I32
; ISEL: SAVIO_SV_MUL_V4I32
; ISEL: SAVIO_SV_SHL_IMM_V4I32
; ISEL: SAVIO_SV_LSHR_IMM_V4I32
; ISEL: SAVIO_SV_ASHR_IMM_V4I32

; CHECK-LABEL: f_add:
; CHECK: add.4s v0, v0, v1
; CHECK-NOT: SAVIO_
define <4 x i32> @f_add(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_sub:
; CHECK: sub.4s v0, v0, v1
define <4 x i32> @f_sub(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.sub.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_mul:
; CHECK: mul.4s v0, v0, v1
define <4 x i32> @f_mul(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.mul.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_and:
; CHECK: and.16b v0, v0, v1
define <4 x i32> @f_and(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.and.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_or:
; CHECK: orr.16b v0, v0, v1
define <4 x i32> @f_or(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.or.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_xor:
; CHECK: eor.16b v0, v0, v1
define <4 x i32> @f_xor(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.xor.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; Variable shift counts.

; CHECK-LABEL: f_shl:
; CHECK-NOT: neg.4s
; CHECK: ushl.4s v0, v0, v1
define <4 x i32> @f_shl(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.shl.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_lshr:
; CHECK: neg.4s v1, v1
; CHECK-NEXT: ushl.4s v0, v0, v1
define <4 x i32> @f_lshr(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.lshr.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: f_ashr:
; CHECK: neg.4s v1, v1
; CHECK-NEXT: sshl.4s v0, v0, v1
define <4 x i32> @f_ashr(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.ashr.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; Constant (splat) shift counts use the immediate forms, no neg.

; CHECK-LABEL: f_shl_imm:
; CHECK-NOT: neg.4s
; CHECK: shl.4s v0, v0, #7
define <4 x i32> @f_shl_imm(<4 x i32> %a) {
  %r = call <4 x i32> @llvm.savio.sv.shl.v4i32(<4 x i32> %a, <4 x i32> splat (i32 7))
  ret <4 x i32> %r
}

; CHECK-LABEL: f_lshr_imm:
; CHECK-NOT: neg.4s
; CHECK: ushr.4s v0, v0, #13
define <4 x i32> @f_lshr_imm(<4 x i32> %a) {
  %r = call <4 x i32> @llvm.savio.sv.lshr.v4i32(<4 x i32> %a, <4 x i32> splat (i32 13))
  ret <4 x i32> %r
}

; CHECK-LABEL: f_ashr_imm:
; CHECK-NOT: neg.4s
; CHECK: sshr.4s v0, v0, #31
define <4 x i32> @f_ashr_imm(<4 x i32> %a) {
  %r = call <4 x i32> @llvm.savio.sv.ashr.v4i32(<4 x i32> %a, <4 x i32> splat (i32 31))
  ret <4 x i32> %r
}

; A zero count is not an immediate form (ushr #0 does not encode); the
; variable form is still correct.

; CHECK-LABEL: f_lshr_zero:
; CHECK-NOT: SAVIO_
; CHECK: ret
define <4 x i32> @f_lshr_zero(<4 x i32> %a) {
  %r = call <4 x i32> @llvm.savio.sv.lshr.v4i32(<4 x i32> %a, <4 x i32> zeroinitializer)
  ret <4 x i32> %r
}
