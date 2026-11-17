; REQUIRES: asserts
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -debug-only=savio -pass-remarks=savio -S %s 2>&1 | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -pass-remarks-output=%t.yaml -disable-output %s
; RUN: FileCheck %s --check-prefix=YAML --input-file=%t.yaml
; One loop with a scalar urem chain (Singleton Vectorize candidate) and an independent vectorized <4 x i32> chain
; (Scalarize candidate): both directions are candidates of one region, forcing moves both, and no group is in both
; directions: the scalar slice is "Promoted" and rewritten, the vector slice is "Scalarized" (reported; the IR of the
; vector chain is unchanged until the rewrite exists).
; CHECK-DAG: savio: group of 2 ops (slice, class [[S:[0-9]+]]) in %loop
; CHECK-DAG: savio: group of 2 ops (scalarize slice, class [[V:[0-9]+]]) in %loop
; CHECK-DAG: remark: {{.*}}: T {{.*}} of slice [[S]]
; CHECK-DAG: remark: {{.*}}: T {{.*}} of slice [[V]]
; CHECK: define void @f(
; The scalar chain was rewritten, the vector chain was not.
; CHECK: call <4 x i32> @llvm.savio.sv.
; CHECK: mul <4 x i32>
; CHECK: xor <4 x i32>
; YAML-DAG: Name:{{ +}}Promoted
; YAML-DAG: Name:{{ +}}Scalarized
target triple = "arm64-apple-macos"

define void @f(ptr noalias %a, ptr noalias %b, ptr noalias %c, ptr noalias %d, i32 %dv, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %sa = load i32, ptr %pa, align 4
  %sm = urem i32 %sa, %dv
  %sx = xor i32 %sm, 255
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  store i32 %sx, ptr %pb, align 4
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %va = load <4 x i32>, ptr %pc, align 4
  %vm = mul <4 x i32> %va, splat (i32 9)
  %vx = xor <4 x i32> %vm, splat (i32 15)
  %pd = getelementptr inbounds i32, ptr %d, i64 %i
  store <4 x i32> %vx, ptr %pd, align 4
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
