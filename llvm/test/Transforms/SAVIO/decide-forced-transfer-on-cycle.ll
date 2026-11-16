; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v-wide -savio-force -pass-remarks=savio -disable-output %s 2>&1 | FileCheck %s
; acc = fshl(acc, 5) ^ v. The xor is forced into the vector domain, so its input
; %rot needs a GprToVec transfer and its result (used by the fshl through the
; phi) a VecToGpr transfer: both lie on the accumulator cycle. The cycle is
; fshl + GprToVec + vector xor + VecToGpr, so T rises from the scalar cycle
; (fshl + xor = 2) to 1 + 1 + 1 + 1 = 4.
; CHECK: remark: {{.*}}T 2.00 -> 4.00 (recurrence)
target triple = "arm64-apple-macos"

declare i32 @llvm.fshl.i32(i32, i32, i32)

define i32 @f(ptr %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %v = load i32, ptr %pb
  %rot = call i32 @llvm.fshl.i32(i32 %acc, i32 %acc, i32 5)
  %acc.next = xor i32 %rot, %v
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.next
}
