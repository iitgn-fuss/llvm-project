; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-bitcount -S %s | FileCheck %s
; RUN: opt -passes=savio-ir,verify -savio-force -mtriple=x86_64-unknown-linux-gnu -mcpu=skylake-avx512 -savio-port-info=%S/Inputs/port-info -savio-cpu=test-avx512-bitcount -S %s | FileCheck %s --check-prefix=CHAIN
; Relies on -savio-force. A serial rotate-xor chain (acc = rotl(acc, 5) ^ b[i+k], four times: fshl has no vector form,
; so the chain is pinned scalar) feeds four llvm.ctpop.i32 whose results are only stored. The four ctpop are four
; instances of one slice and each is Singleton Vectorized: the chain value goes into lane 0 (gpr_to_vec), the
; llvm.savio.sv.ctpop intrinsic takes one operand, lane 0 of the result is stored. The chain itself stays scalar.
; CHECK-LABEL: define void @ctpop4(
; CHECK:       loop:
; CHECK-NOT:   call i32 @llvm.ctpop.i32
; CHECK-COUNT-4: call <4 x i32> @llvm.savio.sv.ctpop.v4i32(<4 x i32> {{%.*}})
; CHECK:       extractelement <4 x i32> {{%.*}}, i32 0
; CHECK-NOT:   call i32 @llvm.ctpop.i32
; CHECK-LABEL: define i32 @carried(
; CHECK-NOT:   call {{.*}}@llvm.savio
; CHECK:       call i32 @llvm.ctpop.i32
; CHECK-NOT:   call {{.*}}@llvm.savio
; The chain (rotate, xor) is not rewritten: the four xor stay, no sv.xor / sv.fshl call.
; CHAIN-LABEL: define void @ctpop4(
; CHAIN-COUNT-4: xor i32 %rot
; CHAIN-NOT:   @llvm.savio.sv.xor
; CHAIN-NOT:   @llvm.savio.sv.fshl

declare i32 @llvm.fshl.i32(i32, i32, i32)
declare i32 @llvm.ctpop.i32(i32)

define void @ctpop4(ptr %b, ptr %o, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %a3, %loop ]
  %i0 = or disjoint i64 %i, 0
  %pb0 = getelementptr inbounds i32, ptr %b, i64 %i0
  %b0 = load i32, ptr %pb0
  %rot0 = call i32 @llvm.fshl.i32(i32 %acc, i32 %acc, i32 5)
  %a0 = xor i32 %rot0, %b0
  %c0 = call i32 @llvm.ctpop.i32(i32 %a0)
  %po0 = getelementptr inbounds i32, ptr %o, i64 %i0
  store i32 %c0, ptr %po0
  %i1 = or disjoint i64 %i, 1
  %pb1 = getelementptr inbounds i32, ptr %b, i64 %i1
  %b1 = load i32, ptr %pb1
  %rot1 = call i32 @llvm.fshl.i32(i32 %a0, i32 %a0, i32 5)
  %a1 = xor i32 %rot1, %b1
  %c1 = call i32 @llvm.ctpop.i32(i32 %a1)
  %po1 = getelementptr inbounds i32, ptr %o, i64 %i1
  store i32 %c1, ptr %po1
  %i2 = or disjoint i64 %i, 2
  %pb2 = getelementptr inbounds i32, ptr %b, i64 %i2
  %b2 = load i32, ptr %pb2
  %rot2 = call i32 @llvm.fshl.i32(i32 %a1, i32 %a1, i32 5)
  %a2 = xor i32 %rot2, %b2
  %c2 = call i32 @llvm.ctpop.i32(i32 %a2)
  %po2 = getelementptr inbounds i32, ptr %o, i64 %i2
  store i32 %c2, ptr %po2
  %i3 = or disjoint i64 %i, 3
  %pb3 = getelementptr inbounds i32, ptr %b, i64 %i3
  %b3 = load i32, ptr %pb3
  %rot3 = call i32 @llvm.fshl.i32(i32 %a2, i32 %a2, i32 5)
  %a3 = xor i32 %rot3, %b3
  %c3 = call i32 @llvm.ctpop.i32(i32 %a3)
  %po3 = getelementptr inbounds i32, ptr %o, i64 %i3
  store i32 %c3, ptr %po3
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}

; Review Focus 4: the result of the ctpop is the loop-carried value (s = ctpop(s) + a[i]): the chain is never moved.
define i32 @carried(ptr %b, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %s = phi i32 [ 0, %entry ], [ %s.next, %loop ]
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %v = load i32, ptr %pb
  %c = call i32 @llvm.ctpop.i32(i32 %s)
  %s.next = add i32 %c, %v
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %s.next
}
