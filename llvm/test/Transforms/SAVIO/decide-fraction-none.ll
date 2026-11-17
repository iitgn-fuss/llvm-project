; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks=savio -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks-output=%t.yaml -disable-output %s
; RUN: FileCheck %s --check-prefix=YAML --input-file=%t.yaml
; add_gpr: acc = fshl(acc + b[i]*P2, 13) * P1, as in slice-feeder.ll but without
; -savio-force. The recurrence (add, fshl, mul) is pinned scalar and bounds T;
; the only slice, the feeder b[i]*P2, is off the cycle, so moving it cannot
; lower T: no k of its one instance beats the unchanged loop by the margin.
; CHECK: remark: {{.*}}: T {{[0-9.]+}} (recurrence)
; CHECK-NOT: T {{.*}} ->
; YAML: !Missed
; YAML: Name:{{ *}}NotProfitable
; YAML-NOT: Name:{{ *}}Promoted
target triple = "arm64-apple-macos"

declare i32 @llvm.fshl.i32(i32, i32, i32)

define i32 @add_gpr(ptr %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %v = load i32, ptr %pb
  %m = mul i32 %v, -1640531535
  %s = add i32 %acc, %m
  %r = call i32 @llvm.fshl.i32(i32 %s, i32 %s, i32 13)
  %acc.next = mul i32 %r, 1597334677
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.next
}
