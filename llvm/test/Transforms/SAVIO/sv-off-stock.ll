; RUN: opt -passes='default<O3>' -mtriple=arm64-apple-macos -mcpu=apple-m4 -S %s > %t.a
; RUN: opt -passes='default<O3>' -mtriple=arm64-apple-macos -mcpu=apple-m4 -enable-savio -savio-port-info=%S/../../../../../src/port-info -S %s > %t.b
; RUN: diff %t.a %t.b
; Only address arithmetic and a loop with nothing to gain: SAVIO changes
; nothing, so the output equals the stock pipeline's.
target triple = "arm64-apple-macos"

define void @f(ptr %p, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %q = getelementptr i32, ptr %p, i64 %i
  %v = load i32, ptr %q
  store i32 %v, ptr %q
  %i.next = add i64 %i, 1
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}
