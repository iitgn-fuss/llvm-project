; RUN: opt -passes=savio-ir -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | opt -passes=verify -disable-output
; Tests singleton vectorization of select instructions.
target triple = "x86_64-unknown-linux-gnu"

define void @test_select_i32(i1 %cond, ptr %p, ptr %q, ptr %r) {
; CHECK-LABEL: @test_select_i32(
; CHECK:         [[A:%.*]] = load i32, ptr %p
; CHECK:         [[B:%.*]] = load i32, ptr %q
; CHECK:         [[COND_SPLAT:%.*]] = shufflevector <4 x i1> {{.*}}, <4 x i32> zeroinitializer
; CHECK:         [[VA:%.*]] = insertelement <4 x i32> poison, i32 [[A]], i32 0
; CHECK:         [[VB:%.*]] = insertelement <4 x i32> poison, i32 [[B]], i32 0
; CHECK:         [[SEL:%.*]] = select <4 x i1> [[COND_SPLAT]], <4 x i32> [[VA]], <4 x i32> [[VB]]
; CHECK:         [[E:%.*]] = extractelement <4 x i32> [[SEL]], i32 0
; CHECK:         store i32 [[E]], ptr %r
; CHECK:         ret void
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %sel = select i1 %cond, i32 %a, i32 %b
  store i32 %sel, ptr %r
  ret void
}
