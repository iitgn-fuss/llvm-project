; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -savio-allow-constant-divisors -pass-remarks=savio -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s
; Divisors that are an argument or a constant cost no transfer in the loop, so T
; matches decide-divide-loop.
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
  %r0 = urem i32 %x0, %p
  %pc0 = getelementptr i32, ptr %c, i64 %ia0
  store i32 %r0, ptr %pc0
  %ia1 = add i64 %i, 1
  %pa1 = getelementptr i32, ptr %a, i64 %ia1
  %x1 = load i32, ptr %pa1
  %r1 = urem i32 %x1, %p
  %pc1 = getelementptr i32, ptr %c, i64 %ia1
  store i32 %r1, ptr %pc1
  %ia2 = add i64 %i, 2
  %pa2 = getelementptr i32, ptr %a, i64 %ia2
  %x2 = load i32, ptr %pa2
  %r2 = urem i32 %x2, 7
  %pc2 = getelementptr i32, ptr %c, i64 %ia2
  store i32 %r2, ptr %pc2
  %ia3 = add i64 %i, 3
  %pa3 = getelementptr i32, ptr %a, i64 %ia3
  %x3 = load i32, ptr %pa3
  %r3 = urem i32 %x3, 7
  %pc3 = getelementptr i32, ptr %c, i64 %ia3
  store i32 %r3, ptr %pc3
  %i.next = add i64 %i, 4
  %cmp = icmp ult i64 %i.next, 1024
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
