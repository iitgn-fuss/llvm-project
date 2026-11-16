; RUN: llc -mtriple=arm64-apple-macos -mcpu=apple-m4 < %s | FileCheck %s

; The DAG combiner must not fold extractelement(call(insertelement, ...))
; back into a scalar operation: the call is opaque to it.

declare <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32>, <4 x i32>)

; CHECK-LABEL: f:
; CHECK: add.4s
; CHECK-NOT: add w
define i32 @f(i32 %a, i32 %b) {
  %va = insertelement <4 x i32> poison, i32 %a, i64 0
  %vb = insertelement <4 x i32> poison, i32 %b, i64 0
  %r = call <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32> %va, <4 x i32> %vb)
  %e = extractelement <4 x i32> %r, i64 0
  ret i32 %e
}
