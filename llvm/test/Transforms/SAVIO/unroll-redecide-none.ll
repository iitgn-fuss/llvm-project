; RUN: opt -passes=savio-ir -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1vc -savio-test-redecide-margin=0.5 -pass-remarks=savio -pass-remarks-missed=savio -S %S/unroll-adi-like.ll 2>&1 | FileCheck %s
; The decision on the real unrolled loop can differ from the estimate made on
; the virtual one (the remark then says both). Here the test option raises the
; margin of the second decision so that it moves nothing: the loop is unrolled
; by 4, the remark is NotProfitable and still names the unroll and the
; estimate, and no divide is moved.
; CHECK: remark: {{.*}}: T 8.00 (resource), unrolled by 4 (estimated 3/8 T 5.00, applied none)
; CHECK: loop.epil:
; CHECK-NOT: call <4 x i32> @llvm.savio
