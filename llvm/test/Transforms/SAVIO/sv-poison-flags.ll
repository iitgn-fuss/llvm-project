; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output

; Operations whose scalar poison/undef semantics are described by flags must
; stay scalar: converting only lane 0 to the ordinary vector intrinsic would
; otherwise discard nuw/nsw/exact semantics.
target triple = "arm64-apple-macos"

define void @f(ptr %p, ptr %q, ptr %r) {
; CHECK-LABEL: @f(
; CHECK: %[[A:.*]] = load i32, ptr %p
; CHECK: %[[B:.*]] = load i32, ptr %q
; CHECK: add nuw i32
; CHECK: sub nsw i32
; CHECK: mul nuw nsw i32
; CHECK: lshr exact i32
; CHECK-NOT: llvm.savio
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %n = add nuw i32 %a, 1
  %s = sub nsw i32 %n, %b
  %m = mul nuw nsw i32 %s, 3
  %h = lshr exact i32 %m, 2
  store i32 %h, ptr %r
  ret void
}
