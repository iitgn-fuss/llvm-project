; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | FileCheck %s
; RUN: opt -passes=savio -savio-force -savio-port-info=%S/Inputs/port-info -savio-cpu=test2s1v -S %s | opt -passes=verify -disable-output
; urem_acc_x4: acc = rotl(acc, 5) ^ bucket(i+k), four times. The accumulator
; chain (fshl, xor) is a recurrence with an operation that has no vector
; equivalent (fshl), so it is pinned scalar: no xor goes to the vector side and
; no transfer lands on the cycle. The four bucket computations (mul, and, add,
; urem) are four instances of one slice and each moves.
; CHECK-COUNT-4: call <4 x i32> @llvm.savio.sv.urem
; CHECK-NOT: llvm.savio.sv.xor
; CHECK-COUNT-4: xor i32
target triple = "arm64-apple-macos"

declare i32 @llvm.fshl.i32(i32, i32, i32)

define i32 @urem_acc_x4(ptr %b, ptr %c, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %a3, %loop ]
  %pb0 = getelementptr inbounds i32, ptr %b, i64 %i
  %pc0 = getelementptr inbounds i32, ptr %c, i64 %i
  %b0 = load i32, ptr %pb0
  %c0 = load i32, ptr %pc0
  %m0 = mul i32 %b0, 2654435761
  %k0 = and i32 %c0, 1023
  %d0 = add i32 %k0, 1
  %r0 = urem i32 %m0, %d0
  %i1 = or disjoint i64 %i, 1
  %pb1 = getelementptr inbounds i32, ptr %b, i64 %i1
  %pc1 = getelementptr inbounds i32, ptr %c, i64 %i1
  %b1 = load i32, ptr %pb1
  %c1 = load i32, ptr %pc1
  %m1 = mul i32 %b1, 2654435761
  %k1 = and i32 %c1, 1023
  %d1 = add i32 %k1, 1
  %r1 = urem i32 %m1, %d1
  %i2 = or disjoint i64 %i, 2
  %pb2 = getelementptr inbounds i32, ptr %b, i64 %i2
  %pc2 = getelementptr inbounds i32, ptr %c, i64 %i2
  %b2 = load i32, ptr %pb2
  %c2 = load i32, ptr %pc2
  %m2 = mul i32 %b2, 2654435761
  %k2 = and i32 %c2, 1023
  %d2 = add i32 %k2, 1
  %r2 = urem i32 %m2, %d2
  %i3 = or disjoint i64 %i, 3
  %pb3 = getelementptr inbounds i32, ptr %b, i64 %i3
  %pc3 = getelementptr inbounds i32, ptr %c, i64 %i3
  %b3 = load i32, ptr %pb3
  %c3 = load i32, ptr %pc3
  %m3 = mul i32 %b3, 2654435761
  %k3 = and i32 %c3, 1023
  %d3 = add i32 %k3, 1
  %r3 = urem i32 %m3, %d3
  %rot0 = call i32 @llvm.fshl.i32(i32 %acc, i32 %acc, i32 5)
  %a0 = xor i32 %rot0, %r0
  %rot1 = call i32 @llvm.fshl.i32(i32 %a0, i32 %a0, i32 5)
  %a1 = xor i32 %rot1, %r1
  %rot2 = call i32 @llvm.fshl.i32(i32 %a1, i32 %a1, i32 5)
  %a2 = xor i32 %rot2, %r2
  %rot3 = call i32 @llvm.fshl.i32(i32 %a2, i32 %a2, i32 5)
  %a3 = xor i32 %rot3, %r3
  %i.next = add nuw i64 %i, 4
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %a3
}
