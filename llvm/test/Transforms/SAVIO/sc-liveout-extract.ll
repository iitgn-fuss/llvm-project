; RUN: opt -passes=savio-ir -savio-port-info=%S/../../../../../src/port-info -savio-cpu=emeraldrapids -savio-force -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; REQUIRES: asserts
; A Scalarize live-out whose only users outside the group are constant-lane extractelements (what SLP leaves): on
; emeraldrapids those extracts are charged as out-of-group instructions (charge_out_of_group), and the backend folds an
; extract of an insert into the scalar, so the lane_insert that rebuilds the vector is not charged on top (@f has no
; "transfer lane_insert" line in its debug output; the four extracts are the single charge). @g also passes the vector
; to a call: a real vector user, so the lane_insert is charged (once per promoted estimate).
; CHECK-LABEL: savio: scalarize of mul <4 x i32> keys
; CHECK: savio: out-of-group charge:   %e3 = extractelement <4 x i32> %m, i64 3
; CHECK: savio: estimate (copies 1, promoted): {{.*}}
; CHECK-NOT: transfer lane_insert
; CHECK: savio: estimate (copies 1, promoted 0): {{.*}}
; CHECK-NOT: transfer lane_insert
; CHECK: savio: estimate (copies 1, promoted 0 1): {{.*}}
; CHECK-NOT: transfer lane_insert
; CHECK: savio: estimate (copies 1, promoted 0 1): {{.*}}
; CHECK: savio: scalarize of mul <4 x i32> keys
; CHECK: savio: out-of-group charge:   %e3 = extractelement <4 x i32> %m, i64 3
; CHECK: savio: estimate (copies 1, promoted): {{.*}}
; CHECK: savio: transfer lane_insert v4i32 per-width
; CHECK: savio: estimate (copies 1, promoted 0): {{.*}}
target triple = "x86_64-unknown-linux-gnu"

declare void @use(<4 x i32>)

define void @f(ptr noalias %a, ptr noalias %o, i64 %n) "target-cpu"="emeraldrapids" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %x = load <4 x i32>, ptr %pa, align 16
  %m = mul <4 x i32> %x, splat (i32 9)
  %e0 = extractelement <4 x i32> %m, i64 0
  %e1 = extractelement <4 x i32> %m, i64 1
  %e2 = extractelement <4 x i32> %m, i64 2
  %e3 = extractelement <4 x i32> %m, i64 3
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  %s01 = add i32 %e0, %e1
  %s23 = add i32 %e2, %e3
  %s = add i32 %s01, %s23
  store i32 %s, ptr %po, align 4
  %i.next = add nuw i64 %i, 4
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

define void @g(ptr noalias %a, ptr noalias %o, i64 %n) "target-cpu"="emeraldrapids" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %x = load <4 x i32>, ptr %pa, align 16
  %m = mul <4 x i32> %x, splat (i32 9)
  %e0 = extractelement <4 x i32> %m, i64 0
  %e1 = extractelement <4 x i32> %m, i64 1
  %e2 = extractelement <4 x i32> %m, i64 2
  %e3 = extractelement <4 x i32> %m, i64 3
  call void @use(<4 x i32> %m)
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  %s01 = add i32 %e0, %e1
  %s23 = add i32 %e2, %e3
  %s = add i32 %s01, %s23
  store i32 %s, ptr %po, align 4
  %i.next = add nuw i64 %i, 4
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}
