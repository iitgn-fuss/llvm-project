; RUN: opt -passes=savio-ir,verify -savio-force-count=8 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s
; N < G: the function asks for 512-bit vectors, so the lane group is G = 8, but a <4 x i32> divide has only four lanes:
; one group of min(G, N) = 4 lanes, the sequence fpdiv4.
; CHECK-LABEL: define void @f(
; CHECK: call <4 x i32> @llvm.savio.fpdiv4.udiv.v4i32(<4 x i32> %{{.*}}, <4 x i32> %{{.*}})
; CHECK-NOT: udiv <4 x i32>
define void @f(ptr %p, ptr %q, ptr %r) "prefer-vector-width"="512" {
  %a = load <4 x i32>, ptr %p
  %b = load <4 x i32>, ptr %q
  %c = udiv <4 x i32> %a, %b
  store <4 x i32> %c, ptr %r
  ret void
}
