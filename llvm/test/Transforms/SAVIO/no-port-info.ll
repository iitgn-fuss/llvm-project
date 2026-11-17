; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=nope -pass-remarks-missed=savio -S %s -o %t.ll 2>&1 | FileCheck %s --check-prefix=REMARK
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=nope -S %s | FileCheck %s
; No port-info root at all: the same remark, IR unchanged.
; RUN: opt -passes=savio-ir -savio-cpu=nope -pass-remarks-missed=savio -S %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=REMARK
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=nope -pass-remarks-output=%t.yaml -disable-output %s
; RUN: FileCheck %s --check-prefix=YAML --input-file=%t.yaml
; YAML: --- !Missed
; YAML: Pass:{{ *}}savio
; YAML: Name:{{ *}}NoPortInfo
; YAML-NOT: Name:
target triple = "arm64-apple-macos"

; REMARK: remark: {{.*}}: no port-info for CPU 'nope'
; REMARK-NOT: remark:

; CHECK: define i32 @f(i32 %a, i32 %b) {
; CHECK-NEXT: %c = udiv i32 %a, %b
; CHECK-NEXT: ret i32 %c
define i32 @f(i32 %a, i32 %b) {
  %c = udiv i32 %a, %b
  ret i32 %c
}
