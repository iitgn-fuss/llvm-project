; RUN: opt -passes=savio -savio-port-info=%S/../../../../../src/port-info -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; Single-copy body of the urem accumulator: acc = fshl(acc, 5) ^ (b*M % ((c&1023)+1)).
; The xor is on the accumulator recurrence (through fshl, which has no port-info),
; so a group containing it would put a transfer on the cycle. No such group is
; promoted.
; CHECK: remark: {{.*}}T {{[0-9.]+}} (recurrence)
; CHECK-NOT: llvm.savio.sv.xor
target triple = "arm64-apple-macosx15.0.0"

declare i32 @llvm.fshl.i32(i32, i32, i32)

define i32 @urem_acc_x4(ptr %b, ptr %c, i64 %n) #0 {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %bv = load i32, ptr %pb
  %cv = load i32, ptr %pc
  %m = mul i32 %bv, 1597334677
  %k = and i32 %cv, 1023
  %d = add i32 %k, 1
  %q = urem i32 %m, %d
  %rot = call i32 @llvm.fshl.i32(i32 %acc, i32 %acc, i32 5)
  %acc.next = xor i32 %rot, %q
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.next
}
attributes #0 = { "target-cpu"="apple-m4" }
