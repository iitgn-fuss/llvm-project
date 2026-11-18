; REQUIRES: asserts
; RUN: opt -passes=savio-ir -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -savio-port-info=%S/../../../../../../src/port-info -savio-cpu=emeraldrapids -debug-only=savio -disable-output %s 2>&1 | FileCheck %s --implicit-check-not="no port-info cost for out-of-group {{.*}}@llvm.ct"
; emeraldrapids sets chargeOutOfGroup(): a ctpop / ctlz that is in no group (here the loop-carried ones, which stay
; scalar; the off-chain %w is the group that makes the loop a region) is charged through its 'ctpop i32' / 'ctlz i32' key (POPCNT32rr / LZCNT32rr), not left to the latency-only
; fallback.
; CHECK-DAG: savio: out-of-group charge:   %c = call i32 @llvm.ctpop.i32(i32 %s)
; CHECK-DAG: savio: out-of-group charge:   %z = call i32 @llvm.ctlz.i32(i32 %t, i1 false)

declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.ctlz.i32(i32, i1 immarg)

define i32 @f(ptr %b, ptr %o, i64 %n) "target-cpu"="emeraldrapids" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i32 [ 0, %entry ], [ %s.next, %loop ]
  %t = phi i32 [ 1, %entry ], [ %t.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %v = load i32, ptr %pb
  %c = call i32 @llvm.ctpop.i32(i32 %s)
  %s.next = add i32 %c, %v
  %z = call i32 @llvm.ctlz.i32(i32 %t, i1 false)
  %t.next = add i32 %z, %v
  %w = call i32 @llvm.ctpop.i32(i32 %v)
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store i32 %w, ptr %po
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  %r = add i32 %s.next, %t.next
  ret i32 %r
}
