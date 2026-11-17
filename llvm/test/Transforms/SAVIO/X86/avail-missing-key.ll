; REQUIRES: asserts
; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=haswell -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -debug-only=savio -S %s 2>&1 | FileCheck %s --check-prefix=ALL
; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=haswell -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-no-sv-add -debug-only=savio -S %s 2>&1 | FileCheck %s --check-prefixes=MISS,NOSV
; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=haswell -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-no-fpdiv4-udiv -debug-only=savio -S %s 2>&1 | FileCheck %s --check-prefixes=MISS,NODIV
; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=haswell -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-no-mul-v8i32 -debug-only=savio -S %s 2>&1 | FileCheck %s --check-prefixes=MISS,NOSC
; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=haswell -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-no-lane-insert -debug-only=savio -S %s 2>&1 | FileCheck %s --check-prefixes=MISS,NOINS
; A candidate of a transformation is formed only if every key the transformation needs is costed. Three functions,
; one per transformation (Singleton Vectorize of an i32 add, fp-divide lanes of a <4 x i32> udiv, Scalarize of a
; <8 x i32> mul); each fixture lacks one key, which makes that transformation unavailable (debug line, IR unchanged)
; and leaves the other two alone. The control (test-avx512) has all keys: nothing is reported missing and all
; three functions change.
; ALL-NOT: not available
; ALL-LABEL: define void @sv(
; ALL: @llvm.savio.sv.add.v4i32
; ALL-LABEL: define void @lanes(
; ALL: @llvm.savio.fpdiv4.udiv.v4i32
; ALL-LABEL: define void @sc(
; ALL: load i32
; ALL-NOT: load <8 x i32>

; NOSV: savio: singleton-vectorize of add i32 not available: missing 'sv.add v4i32'
; NODIV: savio: fpdiv-lanes of udiv <4 x i32> not available: missing 'fpdiv4.udiv v4i32'
; NOSC: savio: scalarize of mul <8 x i32> not available: missing 'mul v8i32'
; NOINS: savio: scalarize of mul <8 x i32> not available: missing 'lane_insert'
; MISS-LABEL: define void @sv(
; NOSV: add i32
; NOSV-NOT: llvm.savio.sv
; NODIV-NOT: not available: missing 'sv
; MISS-LABEL: define void @lanes(
; NODIV: udiv <4 x i32>
; NODIV-NOT: fpdiv4
; MISS-LABEL: define void @sc(
; NOSC: mul <8 x i32>
; NOINS: mul <8 x i32>
; NOSC-NOT: load i32
; NOINS-NOT: load i32

define void @sv(ptr %p, ptr %q, ptr %r) {
  %a = load i32, ptr %p
  %b = load i32, ptr %q
  %c = add i32 %a, %b
  store i32 %c, ptr %r
  ret void
}

define void @lanes(ptr %p, ptr %q, ptr %r) {
  %a = load <4 x i32>, ptr %p
  %b = load <4 x i32>, ptr %q
  %c = udiv <4 x i32> %a, %b
  store <4 x i32> %c, ptr %r
  ret void
}

define void @sc(ptr %p, ptr %q, ptr %r) {
  %a = load <8 x i32>, ptr %p
  %b = load <8 x i32>, ptr %q
  %c = mul <8 x i32> %a, %b
  store <8 x i32> %c, ptr %r
  ret void
}
