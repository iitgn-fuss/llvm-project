; REQUIRES: asserts
; RUN: opt -passes=savio-ir -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test3scv -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; A vectorized sum reduction: the vector accumulator phi and the add that closes it are a recurrence, which is pinned
; in the form LLVM chose and is never a Scalarize group, even forced. The multiply that feeds it is not on the cycle
; and may be Scalarized (its lanes go back into a vector for the add: one lane insert per lane).
; CHECK: savio: group of 1 ops (scalarize slice, class {{[0-9]+}}) in %vector.body
; CHECK-NEXT: load: %w
; CHECK-NEXT: liveout: %m
; CHECK-NOT: savio: group
; CHECK-NOT: recurrence
target triple = "arm64-apple-macos"

define i32 @f(ptr noalias %k, i64 %n.vec) {
entry:
  br label %vector.body

vector.body:
  %index = phi i64 [ 0, %entry ], [ %index.next, %vector.body ]
  %acc = phi <4 x i32> [ zeroinitializer, %entry ], [ %sum, %vector.body ]
  %pk = getelementptr inbounds nuw i32, ptr %k, i64 %index
  %w = load <4 x i32>, ptr %pk, align 4
  %m = mul <4 x i32> %w, splat (i32 7)
  %sum = add <4 x i32> %acc, %m
  %index.next = add nuw i64 %index, 4
  %done = icmp eq i64 %index.next, %n.vec
  br i1 %done, label %exit, label %vector.body

exit:
  %r = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %sum)
  ret i32 %r
}

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)
