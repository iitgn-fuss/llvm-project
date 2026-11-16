; RUN: opt -passes='default<O3>' -mtriple=arm64-apple-macos -mcpu=apple-m4 -S %s | FileCheck %s
; Without -enable-savio the pipeline never creates SAVIO intrinsics.
; CHECK-NOT: llvm.savio
target triple = "arm64-apple-macos"

define void @f(ptr %p, ptr %q, ptr %r) {
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %m = mul i32 %a, %b
  %s = lshr i32 %m, 3
  store i32 %s, ptr %r
  ret void
}
