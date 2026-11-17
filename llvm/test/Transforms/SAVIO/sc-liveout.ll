; RUN: opt -passes=savio-ir,verify -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -S %s | FileCheck %s
; A slice value used outside the slice by something that is not a store of it (here a <4 x i32> phi at the exit, via
; LCSSA, and a call): an insertelement chain rebuilds the vector for that user, after the last lane operation.
; CHECK-LABEL: @f(
; CHECK: loop:
; CHECK: [[M0:%.*]] = mul i32
; CHECK: [[M1:%.*]] = mul i32
; CHECK: [[M2:%.*]] = mul i32
; CHECK: [[M3:%.*]] = mul i32
; CHECK: [[I0:%.*]] = insertelement <4 x i32> poison, i32 [[M0]], i64 0
; CHECK: [[I1:%.*]] = insertelement <4 x i32> [[I0]], i32 [[M1]], i64 1
; CHECK: [[I2:%.*]] = insertelement <4 x i32> [[I1]], i32 [[M2]], i64 2
; CHECK: [[I3:%.*]] = insertelement <4 x i32> [[I2]], i32 [[M3]], i64 3
; CHECK: call void @use(<4 x i32> [[I3]])
; CHECK-NOT: mul <4 x i32>
target triple = "arm64-apple-macos"

declare void @use(<4 x i32>)

define void @f(ptr noalias %a, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %x = load <4 x i32>, ptr %pa, align 4
  %m = mul <4 x i32> %x, splat (i32 9)
  %r = xor <4 x i32> %m, splat (i32 15)
  call void @use(<4 x i32> %m)
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
