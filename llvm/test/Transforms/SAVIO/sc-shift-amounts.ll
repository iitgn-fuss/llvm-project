; RUN: opt -passes=savio-ir,verify -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -S %s | FileCheck %s
; Non-splat shift amounts: each lane of the Scalarized shift takes the matching lane of %amt, extracted (it is a
; function argument defined outside the loop, so the extracts are hoisted to the preheader); flags are copied.
; CHECK-LABEL: @f(
; CHECK: entry:
; CHECK: [[A0:%.*]] = extractelement <4 x i32> %amt, i64 0
; CHECK: [[A1:%.*]] = extractelement <4 x i32> %amt, i64 1
; CHECK: [[A2:%.*]] = extractelement <4 x i32> %amt, i64 2
; CHECK: [[A3:%.*]] = extractelement <4 x i32> %amt, i64 3
; CHECK: loop:
; CHECK: lshr exact i32 {{%.*}}, [[A0]]
; CHECK: lshr exact i32 {{%.*}}, [[A1]]
; CHECK: lshr exact i32 {{%.*}}, [[A2]]
; CHECK: lshr exact i32 {{%.*}}, [[A3]]
; CHECK-NOT: <4 x i32>
target triple = "arm64-apple-macos"

define void @f(ptr noalias %a, ptr noalias %b, <4 x i32> %amt, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %x = load <4 x i32>, ptr %pa, align 4
  %s = lshr exact <4 x i32> %x, %amt
  %r = xor <4 x i32> %s, splat (i32 7)
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  store <4 x i32> %r, ptr %pb, align 4
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
