; RUN: opt -passes=savio-ir,verify -savio-force-count=8 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s --check-prefix=G8
; RUN: opt -passes=savio-ir,verify -savio-force-count=4 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -mattr=-avx512f -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s --check-prefix=G4
; RUN: opt -passes=savio-ir,verify -savio-force-count=8 -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -mattr=-avx512f -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512 -S %s | FileCheck %s --check-prefix=G4ALL
; The lane group G is min(vector_width_bits, the target's vector register width) / 64: the function asks for 512-bit
; vectors, so a <8 x i32> divide has one group of eight lanes. Without AVX-512 the registers are 256 bits, G = 4: the
; divide has two groups and forcing four lanes moves the first (lanes 4..7 stay integer divides).
; G8-LABEL: define void @f(
; G8: call <8 x i32> @llvm.savio.fpdiv8.udiv.v8i32(<8 x i32> %{{.*}}, <8 x i32> %{{.*}})
; G8-NOT: udiv <8 x i32>
; G4-LABEL: define void @f(
; G4: call <8 x i32> @llvm.savio.fpdiv4.udiv.v8i32(<8 x i32> %{{.*}}, <8 x i32> %{{.*}})
; G4-NOT: fpdiv8
; G4: extractelement <8 x i32> %{{.*}}, i32 4
; G4: udiv i32
; G4: insertelement <8 x i32> %{{.*}}, i32 %{{.*}}, i32 4
; G4: udiv i32
; G4: insertelement <8 x i32> %{{.*}}, i32 %{{.*}}, i32 7
; G4-NOT: udiv <8 x i32>
; Eight lanes without AVX-512: both groups, one fpdiv8.
; G4ALL: call <8 x i32> @llvm.savio.fpdiv8.udiv.v8i32(
define void @f(ptr %b, ptr %c, ptr %o, i64 %n) #0 {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %pc = getelementptr inbounds i32, ptr %c, i64 %i
  %vb = load <8 x i32>, ptr %pb
  %vc = load <8 x i32>, ptr %pc
  %r = udiv <8 x i32> %vb, %vc
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store <8 x i32> %r, ptr %po
  %i.next = add nuw i64 %i, 8
  %done = icmp uge i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

attributes #0 = { "prefer-vector-width"="512" "min-legal-vector-width"="0" }
