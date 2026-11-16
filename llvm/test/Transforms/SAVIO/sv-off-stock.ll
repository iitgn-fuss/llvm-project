; RUN: opt -passes='default<O3>' -mtriple=arm64-apple-macos -mcpu=apple-m4 -S %s > %t.a
; RUN: opt -passes='default<O3>' -mtriple=arm64-apple-macos -mcpu=apple-m4 -enable-savio -savio-port-info=%S/../../../../../src/port-info -S %s > %t.b
; RUN: diff %t.a %t.b
; RUN: FileCheck %s --input-file=%t.a
; RUN: opt -passes='default<O3>' -mtriple=arm64-apple-macos -mcpu=apple-m4 -enable-savio -savio-port-info=%S/../../../../../src/port-info -pass-remarks-missed=savio -disable-output %s 2>&1 | FileCheck %s --check-prefix=REM
; An FNV-style hash: add, xor and mul i32 work on a serial recurrence, which the
; stock pipeline neither vectorizes nor removes. SAVIO evaluates the loop and
; rejects it (the recurrence limits T), so the output equals the stock
; pipeline's.
; The loop survives -O3, so the diff compares real work:
; CHECK: phi i32
; CHECK: xor i32
; CHECK: mul i32
; REM: remark: {{.*}}T {{[0-9.]+}} (recurrence)
target triple = "arm64-apple-macos"

define i32 @f(ptr %p, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %h = phi i32 [ 2166136261, %entry ], [ %h.next, %loop ]
  %q = getelementptr i32, ptr %p, i64 %i
  %v = load i32, ptr %q
  %x = xor i32 %h, %v
  %m = mul i32 %x, 16777619
  %h.next = add i32 %m, 7
  %i.next = add i64 %i, 1
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret i32 %h.next
}
