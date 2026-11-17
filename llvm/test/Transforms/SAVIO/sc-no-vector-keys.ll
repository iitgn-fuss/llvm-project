; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s 2>&1 | FileCheck %s --check-prefix=IR
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s --check-prefix=FORCE
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks-output=%t.yaml -disable-output %s
; RUN: FileCheck %s --check-prefix=YAML --input-file=%t.yaml
; An -O3-shaped loop: a vectorized body (<4 x i32> load, mul, xor, lshr, store) next to a scalar tail copy of the same
; chain. The test2s1v fixture is Part-2 format: it has no "<op> v4i32" / "<op> i32" vector probe keys and no
; lane_extract / lane_insert transfers, so vector slices cannot be costed and nothing is Scalarized. Singleton Vectorize
; behaves as before: the scalar copy is decided on its own (one remark per region) and the vector body is untouched.
; CHECK: remark: {{.*}}: T 3.75 (dispatch)
; CHECK-NOT: remark:
; IR-LABEL: define void @f(
; IR:         load <4 x i32>
; IR-NEXT:    mul <4 x i32>
; IR-NEXT:    xor <4 x i32>
; IR-NEXT:    lshr <4 x i32>
; IR-NEXT:    store <4 x i32>
; IR:         load i32
; IR-NEXT:    mul i32
; IR-NEXT:    xor i32
; IR-NEXT:    lshr i32
; IR-NEXT:    store i32
; IR-NOT:     @llvm.savio
; IR-NOT:     insertelement
; IR-NOT:     extractelement
; Forced: Singleton Vectorize still rewrites the scalar copy (lane-0 calls), the vector body keeps its <4 x i32> ops.
; FORCE-LABEL: define void @f(
; FORCE:         load <4 x i32>
; FORCE-NEXT:    mul <4 x i32>
; FORCE-NEXT:    xor <4 x i32>
; FORCE-NEXT:    lshr <4 x i32>
; FORCE-NEXT:    store <4 x i32>
; FORCE:         call <4 x i32> @llvm.savio.sv.mul.v4i32
; FORCE:         call <4 x i32> @llvm.savio.sv.xor.v4i32
; FORCE:         call <4 x i32> @llvm.savio.sv.lshr.v4i32
; Even forced, the missing vector keys, scalar-lane keys and lane transfers make every Scalarize assignment uncostable:
; no `Scalarized` remark, only Singleton Vectorize's `Promoted` for the scalar copy. (If Scalarize ignored a missing key
; this would fail: the vector slice would be decided and reported.)
; YAML-NOT: Name:{{ +}}Scalarized
; YAML:     Name:{{ +}}Promoted
; YAML-NOT: Name:{{ +}}Scalarized
target triple = "arm64-apple-macos"

define void @f(ptr %a, ptr %b, ptr %c, ptr %d, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %va = load <4 x i32>, ptr %pa, align 4
  %vm = mul <4 x i32> %va, splat (i32 -2048144789)
  %vx = xor <4 x i32> %vm, %va
  %vs = lshr <4 x i32> %vx, splat (i32 13)
  store <4 x i32> %vs, ptr %pb, align 4
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %pd = getelementptr inbounds i32, ptr %d, i64 %i
  %sa = load i32, ptr %pc, align 4
  %sm = mul i32 %sa, -2048144789
  %sx = xor i32 %sm, %sa
  %ss = lshr i32 %sx, 13
  store i32 %ss, ptr %pd, align 4
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
