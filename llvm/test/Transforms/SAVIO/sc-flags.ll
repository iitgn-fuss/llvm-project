; RUN: opt -passes=savio,verify -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -S %s | FileCheck %s
; nsw/nuw, nuw and exact are kept on every lane of the scalar operations.
; CHECK-LABEL: @f(
; CHECK-COUNT-4: add nuw nsw i32
; CHECK-COUNT-4: shl nuw i32
; CHECK-COUNT-4: lshr exact i32
; CHECK-NOT: <4 x i32>
target triple = "arm64-apple-macos"

define void @f(ptr noalias %a, ptr noalias %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %x = load <4 x i32>, ptr %pa, align 4
  %s = add nsw nuw <4 x i32> %x, splat (i32 3)
  %t = shl nuw <4 x i32> %s, splat (i32 2)
  %u = lshr exact <4 x i32> %t, splat (i32 2)
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  store <4 x i32> %u, ptr %pb, align 4
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
