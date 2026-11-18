; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/../../../../../src/port-info -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-port-info=%S/../../../../../src/port-info -S %s | FileCheck %s
; Review Focus 3: apple-m4 has no 'sv.ctpop' / 'sv.ctlz' key (the M4 vector unit has no per-lane count equivalent
; SAVIO models), so a ctpop / ctlz is never a candidate there, forced or not: the calls stay and no llvm.savio call
; appears.
; CHECK-LABEL: define void @f(
; CHECK:       call i32 @llvm.ctpop.i32
; CHECK:       call i32 @llvm.ctlz.i32
; CHECK-NOT:   llvm.savio
target triple = "arm64-apple-macosx15.0.0"

declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.ctlz.i32(i32, i1 immarg)

define void @f(ptr %b, ptr %o, ptr %o2, i64 %n) #0 {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %v = load i32, ptr %pb
  %c = call i32 @llvm.ctpop.i32(i32 %v)
  %z = call i32 @llvm.ctlz.i32(i32 %v, i1 false)
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store i32 %c, ptr %po
  %po2 = getelementptr inbounds i32, ptr %o2, i64 %i
  store i32 %z, ptr %po2
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
attributes #0 = { "target-cpu"="apple-m4" }
