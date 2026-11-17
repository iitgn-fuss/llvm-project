; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/bad/port-info -savio-cpu=badcpu -pass-remarks-missed=savio -S %s -o %t.ll 2>&1 | FileCheck %s --check-prefix=REMARK
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/bad/port-info -savio-cpu=badcpu -S %s | FileCheck %s
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/bad/port-info -savio-cpu=badcpu -pass-remarks-output=%t.yaml -disable-output %s
; RUN: FileCheck %s --check-prefix=YAML --input-file=%t.yaml
; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/bad/port-info -savio-cpu=badcpu -disable-output %s 2>&1 | FileCheck %s --check-prefix=WARN
; WARN: warning: savio: invalid port-info for CPU 'badcpu': {{.*}}ops.json: {{.*}}unknown class 'bogus'
; WARN-NOT: warning:
; YAML: --- !Missed
; YAML: Pass:{{ *}}savio
; YAML: Name:{{ *}}PortInfoError
; YAML-NOT: Name:
target triple = "x86_64-linux-gnu"

; REMARK: remark: {{.*}}: ops.json: {{.*}}unknown class 'bogus'{{.*}}
; REMARK-NOT: remark:

; CHECK: define i32 @f(i32 %a, i32 %b) {
; CHECK-NEXT: %c = udiv i32 %a, %b
; CHECK-NEXT: ret i32 %c
define i32 @f(i32 %a, i32 %b) {
  %c = udiv i32 %a, %b
  ret i32 %c
}
