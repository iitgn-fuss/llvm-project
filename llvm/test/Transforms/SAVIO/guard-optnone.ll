; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; optnone functions are left alone even with -savio-force.
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
attributes #0 = { noinline optnone }
