; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 -stop-after=aarch64-savio-expand < %s | FileCheck %s

; After aarch64-savio-expand no SAVIO_SV_ pseudo remains, and the expansion is
; still in SSA form (virtual registers).

declare <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32>, <4 x i32>)
declare <4 x i32> @llvm.savio.sv.lshr.v4i32(<4 x i32>, <4 x i32>)

; CHECK-LABEL: name: f_urem
; CHECK-NOT: SAVIO_SV_
; CHECK: USHLLv2i32_shift
; CHECK: UCVTFv1i64
; CHECK: FDIVDrr
; CHECK: FCVTZUv1i64
; CHECK: MLSv2i32
; CHECK-NOT: SAVIO_SV_
define <4 x i32> @f_urem(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.urem.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}

; CHECK-LABEL: name: f_lshr
; CHECK-NOT: SAVIO_SV_
; CHECK: NEGv4i32
; CHECK: USHLv4i32
define <4 x i32> @f_lshr(<4 x i32> %a, <4 x i32> %b) {
  %r = call <4 x i32> @llvm.savio.sv.lshr.v4i32(<4 x i32> %a, <4 x i32> %b)
  ret <4 x i32> %r
}
