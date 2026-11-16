; RUN: opt -passes=savio -savio-force-count=2 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s --check-prefix=K2
; RUN: opt -passes=savio -savio-force-count=4 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s --check-prefix=K4
; RUN: opt -passes=savio -savio-force-count=0 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s --check-prefix=K0
; RUN: opt -passes=savio -savio-force-count=2 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; RUN: opt -passes=savio -savio-force-count=4 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; RUN: opt -passes=savio -savio-force-count=2 -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s -pass-remarks=savio 2>&1 | FileCheck %s --check-prefix=REM
; The lanes of a urem <4 x i32> are two instances (lane pairs {0,1} and {2,3})
; of one slice class; -savio-force-count counts lanes for such a class.
;
; Two lanes: one fpdiv2 for lanes {0,1}; lanes 2 and 3 stay scalar urem
; and are inserted.
; K2-LABEL: define void @f(
; K2: call <4 x i32> @llvm.savio.fpdiv2.urem.v4i32(<4 x i32> %{{.*}}, <4 x i32> %{{.*}})
; K2-NOT: llvm.savio.fpdiv2
; K2: %[[A2:.*]] = extractelement <4 x i32> %{{.*}}, i32 2
; K2: %[[B2:.*]] = extractelement <4 x i32> %{{.*}}, i32 2
; K2: urem i32 %[[A2]], %[[B2]]
; K2: insertelement <4 x i32> %{{.*}}, i32 %{{.*}}, i32 2
; K2: urem i32
; K2: insertelement <4 x i32> %{{.*}}, i32 %{{.*}}, i32 3
; K2-NOT: urem <4 x i32>
; Four lanes: two fpdiv2, the second on lanes 2 and 3 shuffled down, and a
; shuffle that joins the halves; no scalar urem.
; K4-LABEL: define void @f(
; K4: call <4 x i32> @llvm.savio.fpdiv2.urem.v4i32
; K4: shufflevector <4 x i32> %{{.*}}, <4 x i32> poison, <4 x i32> <i32 2, i32 3, i32 poison, i32 poison>
; K4: shufflevector <4 x i32> %{{.*}}, <4 x i32> poison, <4 x i32> <i32 2, i32 3, i32 poison, i32 poison>
; K4: call <4 x i32> @llvm.savio.fpdiv2.urem.v4i32
; K4: shufflevector <4 x i32> %{{.*}}, <4 x i32> %{{.*}}, <4 x i32> <i32 0, i32 1, i32 4, i32 5>
; K4-NOT: urem i32
; K4-NOT: urem <4 x i32>
; K0-NOT: llvm.savio
; K0: urem <4 x i32>
; REM: remark: {{.*}}2/4 of slice
target triple = "arm64-apple-macos"

define void @f(ptr %b, ptr %c, ptr %o, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %vb = load <4 x i32>, ptr %pb
  %vc = load <4 x i32>, ptr %pc
  %r = urem <4 x i32> %vb, %vc
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store <4 x i32> %r, ptr %po
  %i.next = add nuw i64 %i, 4
  %done = icmp uge i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
