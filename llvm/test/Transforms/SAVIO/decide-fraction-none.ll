; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks-missed=savio -pass-remarks=savio -S %s 2>&1 | FileCheck %s
; The add_gpr body: one add slice whose operands come from GPRs. Moving it
; costs transfers that outweigh the one scalar add it frees, so no fraction of
; one instance beats the unchanged loop.
; CHECK: remark: {{.*}} T {{[0-9.]+}} ({{.*}})
; CHECK-NOT: Promoted
; CHECK-NOT: llvm.savio.sv
target triple = "arm64-apple-macos"

define void @f(ptr %b, ptr %o, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %x = load i32, ptr %pb
  %r = add i32 %x, 7
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store i32 %r, ptr %po
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
