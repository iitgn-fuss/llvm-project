; RUN: opt -passes=savio,verify -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -S %s | FileCheck %s
; The load of the Scalarized copy is also used by a second chain that stays a vector (here: a store, which is not in
; the slice), so the slice does not own it: the vector load is kept and its four lanes are extracted; no second load.
; CHECK-LABEL: @f(
; CHECK: [[V:%.*]] = load <4 x i32>, ptr %pa
; CHECK: store <4 x i32> [[V]], ptr %pc
; CHECK: extractelement <4 x i32> [[V]], i64 0
; CHECK: extractelement <4 x i32> [[V]], i64 1
; CHECK: extractelement <4 x i32> [[V]], i64 2
; CHECK: extractelement <4 x i32> [[V]], i64 3
; CHECK-NOT: load
; CHECK: store i32
target triple = "arm64-apple-macos"

define void @f(ptr noalias %a, ptr noalias %b, ptr noalias %c, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %va = load <4 x i32>, ptr %pa, align 4
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  store <4 x i32> %va, ptr %pc, align 4
  %vm = mul <4 x i32> %va, splat (i32 9)
  %vx = xor <4 x i32> %vm, splat (i32 15)
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  store <4 x i32> %vx, ptr %pb, align 4
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
