; RUN: opt -passes=savio -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -pass-remarks=savio -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s
; Four independent urems: two go to the vector divider.
; CHECK-COUNT-2: remark: {{.*}}T 16.00 -> 8.00 (resource)
; CHECK-NOT: remark:
target triple = "arm64-apple-macos"

define void @f(ptr %a, ptr %b, ptr %c, i32 %p) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %ia0 = add i64 %i, 0
  %pa0 = getelementptr i32, ptr %a, i64 %ia0
  %x0 = load i32, ptr %pa0
  %pb0 = getelementptr i32, ptr %b, i64 %ia0
  %y0 = load i32, ptr %pb0
  %r0 = urem i32 %x0, %y0
  %pc0 = getelementptr i32, ptr %c, i64 %ia0
  store i32 %r0, ptr %pc0
  %ia1 = add i64 %i, 1
  %pa1 = getelementptr i32, ptr %a, i64 %ia1
  %x1 = load i32, ptr %pa1
  %pb1 = getelementptr i32, ptr %b, i64 %ia1
  %y1 = load i32, ptr %pb1
  %r1 = urem i32 %x1, %y1
  %pc1 = getelementptr i32, ptr %c, i64 %ia1
  store i32 %r1, ptr %pc1
  %ia2 = add i64 %i, 2
  %pa2 = getelementptr i32, ptr %a, i64 %ia2
  %x2 = load i32, ptr %pa2
  %pb2 = getelementptr i32, ptr %b, i64 %ia2
  %y2 = load i32, ptr %pb2
  %r2 = urem i32 %x2, %y2
  %pc2 = getelementptr i32, ptr %c, i64 %ia2
  store i32 %r2, ptr %pc2
  %ia3 = add i64 %i, 3
  %pa3 = getelementptr i32, ptr %a, i64 %ia3
  %x3 = load i32, ptr %pa3
  %pb3 = getelementptr i32, ptr %b, i64 %ia3
  %y3 = load i32, ptr %pb3
  %r3 = urem i32 %x3, %y3
  %pc3 = getelementptr i32, ptr %c, i64 %ia3
  store i32 %r3, ptr %pc3
  %i.next = add i64 %i, 4
  %cmp = icmp ult i64 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
