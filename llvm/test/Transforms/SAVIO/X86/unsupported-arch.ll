; RUN: sed 's/@TRIPLE@/i686-linux-gnu/' %s > %t.i386.ll
; i686 defaults to no SSE, where SAVIO bails before looking up port-info; give it vector registers.
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -mcpu=pentium4 -savio-cpu=nope -pass-remarks-missed=savio -S %t.i386.ll -o %t.out1.ll 2>&1 | FileCheck %s --check-prefix=I386
; RUN: FileCheck %s --input-file=%t.out1.ll
; RUN: sed 's/@TRIPLE@/riscv64-unknown-linux-gnu/' %s > %t.rv.ll
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -mattr=+v -savio-cpu=nope -pass-remarks-missed=savio -S %t.rv.ll -o %t.out2.ll 2>&1 | FileCheck %s --check-prefix=RV64
; RUN: FileCheck %s --input-file=%t.out2.ll
; (riscv64 likewise needs +v: with a RISC-V target built in, it has no vector registers without it and SAVIO bails first.)
; An architecture with no port-info directory: the remark names the architecture, IR unchanged.
target triple = "@TRIPLE@"

; I386: remark: {{.*}}: no port-info for architecture 'i386'
; I386-NOT: remark:
; RV64: remark: {{.*}}: no port-info for architecture 'riscv64'
; RV64-NOT: remark:

; CHECK: define i32 @f(i32 %a, i32 %b){{.*}} {
; CHECK-NEXT: %c = udiv i32 %a, %b
; CHECK-NEXT: ret i32 %c
define i32 @f(i32 %a, i32 %b) {
  %c = udiv i32 %a, %b
  ret i32 %c
}
