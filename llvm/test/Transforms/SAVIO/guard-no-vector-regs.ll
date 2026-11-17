; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; Without vector registers the intrinsics cannot be selected: leave the function alone.
; CHECK-NOT: llvm.savio
target triple = "arm64-apple-macos"
define void @f(ptr %p, ptr %q, ptr %r) #0 {
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %m = mul i32 %a, %b
  %s = lshr i32 %m, 3
  store i32 %s, ptr %r
  ret void
}
attributes #0 = { "target-features"="+general-regs-only,-neon,-fp-armv8" }
