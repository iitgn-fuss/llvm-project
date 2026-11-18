; RUN: opt -passes=savio-ir,verify -savio-force-count=8 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force-count=8 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=KEYS
; REQUIRES: asserts
; N < G: the function asks for 512-bit vectors, so the lane group is G = 8, but a <4 x i32> divide has only four lanes:
; one group of min(G, N) = 4 lanes, the sequence fpdiv4. The companion @g pins G = 8: the debug output (second RUN) shows
; that the <8 x i32> divide with the same attribute is costed with the 512-bit key fpdiv8.512 (one group of eight
; lanes), which only a lane group of 512 bits uses; the IR alone would not tell G = 8 from G = 4.
; KEYS: savio: fpdiv-lanes of udiv <4 x i32> keys: 'udiv v4i32' 'fpdiv4.udiv v4i32'
; KEYS: savio: fpdiv-lanes of udiv <8 x i32> keys: 'udiv v8i32' 'fpdiv8.512.udiv v8i32'
; CHECK-LABEL: define void @f(
; CHECK: call <4 x i32> @llvm.savio.fpdiv4.udiv.v4i32(<4 x i32> %{{.*}}, <4 x i32> %{{.*}})
; CHECK-NOT: udiv <4 x i32>
; CHECK-LABEL: define void @g(
; CHECK: call <8 x i32> @llvm.savio.fpdiv8.udiv.v8i32(<8 x i32> %{{.*}}, <8 x i32> %{{.*}})
; CHECK-NOT: udiv <8 x i32>
define void @f(ptr %p, ptr %q, ptr %r) "prefer-vector-width"="512" {
  %a = load <4 x i32>, ptr %p
  %b = load <4 x i32>, ptr %q
  %c = udiv <4 x i32> %a, %b
  store <4 x i32> %c, ptr %r
  ret void
}

define void @g(ptr %p, ptr %q, ptr %r) "prefer-vector-width"="512" {
  %a = load <8 x i32>, ptr %p
  %b = load <8 x i32>, ptr %q
  %c = udiv <8 x i32> %a, %b
  store <8 x i32> %c, ptr %r
  ret void
}
