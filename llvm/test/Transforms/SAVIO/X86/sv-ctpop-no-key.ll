; REQUIRES: asserts
; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -debug-only=savio -S %s 2>&1 | FileCheck %s
; Review Focus 2: test-avx512 has no 'sv.ctpop v4i32' / 'sv.ctlz v4i32' key, so a ctpop / ctlz is not a candidate: the
; debug lines name the missing key (the other keys of the transformation are not looked at) and the IR keeps the
; scalar calls with no llvm.savio call. (The debug output precedes the IR.)
; CHECK: savio: singleton-vectorize of ctpop i32 not available: missing 'sv.ctpop v4i32'
; CHECK: savio: singleton-vectorize of ctlz i32 not available: missing 'sv.ctlz v4i32'
; CHECK-LABEL: define void @f(
; CHECK:       call i32 @llvm.ctpop.i32(i32 %a)
; CHECK:       call i32 @llvm.ctlz.i32(i32 %b, i1 false)
; CHECK-NOT:   llvm.savio

declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.ctlz.i32(i32, i1 immarg)

define void @f(ptr %p, ptr %q, ptr %r, ptr %s) {
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %c = call i32 @llvm.ctpop.i32(i32 %a)
  %d = call i32 @llvm.ctlz.i32(i32 %b, i1 false)
  store i32 %c, ptr %r
  store i32 %d, ptr %s
  ret void
}
