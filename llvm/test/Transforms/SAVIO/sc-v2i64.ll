; RUN: opt -passes=savio,verify -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -S %s | FileCheck %s
; <2 x i64>: eight-byte lanes, a non-splat constant becomes a scalar constant per lane, a loop-invariant vector computed
; in the entry block (%inv, a shuffle) is extracted once in the preheader, and a volatile vector load is
; never split: it stays a vector load and its lanes are extracted.
; CHECK-LABEL: @f(
; CHECK: entry:
; CHECK: [[I0:%.*]] = extractelement <2 x i64> %inv, i64 0
; CHECK: [[I1:%.*]] = extractelement <2 x i64> %inv, i64 1
; CHECK: loop:
; CHECK: load i64, ptr %pa, align 16
; CHECK: [[P:%.*]] = getelementptr i8, ptr %pa, i64 8
; CHECK: load i64, ptr [[P]], align 8
; CHECK: add i64 {{.*}}, 1
; CHECK: add i64 {{.*}}, 2
; CHECK: mul i64 {{.*}}, [[I0]]
; CHECK: mul i64 {{.*}}, [[I1]]
; CHECK: load volatile <2 x i64>, ptr %pv
; CHECK: extractelement <2 x i64> %vv, i64 0
; CHECK: xor i64
; CHECK: extractelement <2 x i64> %vv, i64 1
; CHECK: xor i64
target triple = "arm64-apple-macos"

define void @f(ptr noalias %a, ptr noalias %b, ptr %v, <2 x i64> %s, i64 %n) {
entry:
  %inv = shufflevector <2 x i64> %s, <2 x i64> poison, <2 x i32> <i32 1, i32 0>
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds <2 x i64>, ptr %a, i64 %i
  %x = load <2 x i64>, ptr %pa, align 16
  %c = add <2 x i64> %x, <i64 1, i64 2>
  %m = mul <2 x i64> %c, %inv
  %pv = getelementptr inbounds <2 x i64>, ptr %v, i64 %i
  %vv = load volatile <2 x i64>, ptr %pv, align 16
  %r = xor <2 x i64> %m, %vv
  %pb = getelementptr inbounds <2 x i64>, ptr %b, i64 %i
  store <2 x i64> %r, ptr %pb, align 16
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
