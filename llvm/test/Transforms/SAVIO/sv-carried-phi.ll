; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; Relies on -savio-force: the accumulator group is promoted. The carried phi
; becomes a <4 x i32> phi whose preheader value is inserted in lane 0, and the
; value used after the loop (through an LCSSA phi) is extracted in the exit
; block.
target triple = "arm64-apple-macos"

define i32 @f(ptr %p, i32 %init) {
; CHECK-LABEL: @f(
; CHECK:       entry:
; CHECK-NEXT:    [[INIT:%.*]] = insertelement <4 x i32> poison, i32 %init, i32 0
; CHECK-NEXT:    br label %loop
; CHECK:       loop:
; CHECK:         [[ACC:%.*]] = phi <4 x i32> [ [[INIT]], %entry ], [ [[NEXT:%.*]], %loop ]
; CHECK:         [[NEXT]] = call <4 x i32> @llvm.savio.sv.add.v4i32(<4 x i32> [[ACC]], <4 x i32> {{%.*}})
; CHECK-NOT:     extractelement
; CHECK:       exit:
; CHECK-NEXT:    [[R:%.*]] = extractelement <4 x i32> [[NEXT]], i32 0
; CHECK-NEXT:    ret i32 [[R]]
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ %init, %entry ], [ %acc.next, %loop ]
  %pa = getelementptr i32, ptr %p, i64 %i
  %x = load i32, ptr %pa
  %acc.next = add i32 %acc, %x
  %i.next = add i64 %i, 1
  %cmp = icmp ult i64 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  %r = phi i32 [ %acc.next, %loop ]
  ret i32 %r
}
