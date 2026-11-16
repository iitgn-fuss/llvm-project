; RUN: opt -passes=savio -S %s | FileCheck %s
; RUN: opt -passes='default<O3>' -mtriple=arm64-apple-macos -mcpu=apple-m4 -enable-savio -debug-pass-manager -S %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=PIPE
; RUN: opt -passes='default<O3>' -mtriple=arm64-apple-macos -mcpu=apple-m4 -debug-pass-manager -S %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=OFF
; CHECK: define i32 @f
; PIPE: Running pass: SAVIOPass on f
; OFF-NOT: SAVIOPass
define i32 @f(i32 %a, i32 %b) { %c = add i32 %a, %b
  ret i32 %c }
