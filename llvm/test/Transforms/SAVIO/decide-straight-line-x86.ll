; RUN: opt -passes=savio-ir -savio-port-info=%S/../../../../../src/port-info -savio-cpu=emeraldrapids -pass-remarks=savio -pass-remarks-missed=savio -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; REQUIRES: asserts
; resources.json "critical_path_outside_loops": false (emeraldrapids only): a region outside a loop has no
; recurrence bound, because its critical path overlaps with the neighbouring code (measured on d1_slp_hash4's
; straight-line block, matrix report section 10). A chain of three vector multiplies has a critical path of
; about 30 cycles (3 x VPMULLD 10) but TRec reads 0 and the stock T is the resource bound; moving it to scalar
; (TRes 12) does not pay, so the missed remark is a resource-bound "none". apple-m4 and the fixtures keep the critical path (decide-straight-line.ll).
; CHECK: estimate (copies 1, promoted): TRes 3.015{{.*}} (vec_mul) TRec 0.000000e+00
; CHECK: remark: {{.*}}T 3.01 (resource)
target triple = "x86_64-unknown-linux-gnu"

define void @f(ptr %p, ptr %q) "target-cpu"="emeraldrapids" {
  %a = load <4 x i32>, ptr %p, align 16
  %m1 = mul <4 x i32> %a, <i32 3, i32 5, i32 7, i32 9>
  %x1 = xor <4 x i32> %m1, %a
  %m2 = mul <4 x i32> %x1, <i32 11, i32 13, i32 15, i32 17>
  %x2 = xor <4 x i32> %m2, %x1
  %m3 = mul <4 x i32> %x2, <i32 19, i32 21, i32 23, i32 25>
  store <4 x i32> %m3, ptr %q, align 16
  ret void
}
