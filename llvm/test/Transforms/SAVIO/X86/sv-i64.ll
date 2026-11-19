; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | opt -passes=verify -disable-output
; 64-bit integer arithmetic singleton vectorization lowers to <2 x i64> vector operations.
target triple = "x86_64-unknown-linux-gnu"

; CHECK-LABEL: @test_i64_chain(
; CHECK: insertelement <2 x i64>
; CHECK: call <2 x i64> @llvm.savio.sv.mul.v2i64(
; CHECK: call <2 x i64> @llvm.savio.sv.xor.v2i64(
; CHECK: call <2 x i64> @llvm.savio.sv.lshr.v2i64(
; CHECK: extractelement <2 x i64>
define void @test_i64_chain(ptr %p, ptr %q, ptr %r) {
  %a = load i64, ptr %p
  %b = load i64, ptr %q
  %m = mul i64 %a, %b
  %x = xor i64 %m, %a
  %s = lshr i64 %x, 3
  store i64 %s, ptr %r
  ret void
}

; CHECK-LABEL: @test_i64_add_sub(
; CHECK: insertelement <2 x i64>
; CHECK: call <2 x i64> @llvm.savio.sv.add.v2i64(
; CHECK: call <2 x i64> @llvm.savio.sv.sub.v2i64(
; CHECK: extractelement <2 x i64>
define void @test_i64_add_sub(ptr %p, ptr %q, ptr %r, i64 %c) {
  %a = load i64, ptr %p
  %b = load i64, ptr %q
  %m = add i64 %a, %b
  %x = sub i64 %m, %c
  store i64 %x, ptr %r
  ret void
}
