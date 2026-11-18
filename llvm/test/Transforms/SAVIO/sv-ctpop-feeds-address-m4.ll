; RUN: opt -passes=savio-ir,verify -savio-force -savio-port-info=%S/../../../../../src/port-info -S %s | FileCheck %s
target triple = "arm64-apple-macosx15.0.0"

declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.bswap.i32(i32)

; The count (or the byte swap, the control: not a candidate on any target) is used as a table index, so its value
; reaches an address. The walk of reachesAddress stops at it: the xor and mul before it are not address
; arithmetic and still form one group and are Singleton Vectorized, with or without the count's own key. The count
; itself stays scalar (it reaches an address).
; CHECK-LABEL: define void @pop(
; CHECK:       call <4 x i32> @llvm.savio.sv.xor.v4i32
; CHECK:       call <4 x i32> @llvm.savio.sv.mul.v4i32
; CHECK:       call i32 @llvm.ctpop.i32
; CHECK-NOT:   call <4 x i32> @llvm.savio.sv.ctpop
; CHECK-LABEL: define void @swap(
; CHECK:       call <4 x i32> @llvm.savio.sv.xor.v4i32
; CHECK:       call <4 x i32> @llvm.savio.sv.mul.v4i32
; CHECK:       call i32 @llvm.bswap.i32

define void @pop(ptr %a, ptr %table, ptr %o, i64 %n) #0 {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %v = load i32, ptr %pa
  %x = xor i32 %v, 12345
  %m = mul i32 %x, %v
  %c = call i32 @llvm.ctpop.i32(i32 %m)
  %z = zext i32 %c to i64
  %pt = getelementptr inbounds i32, ptr %table, i64 %z
  %t = load i32, ptr %pt
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store i32 %t, ptr %po
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}

define void @swap(ptr %a, ptr %table, ptr %o, i64 %n) #0 {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %v = load i32, ptr %pa
  %x = xor i32 %v, 12345
  %m = mul i32 %x, %v
  %c = call i32 @llvm.bswap.i32(i32 %m)
  %z = zext i32 %c to i64
  %pt = getelementptr inbounds i32, ptr %table, i64 %z
  %t = load i32, ptr %pt
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store i32 %t, ptr %po
  %i.next = add nuw i64 %i, 1
  %cmp = icmp ult i64 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
attributes #0 = { "target-cpu"="apple-m4" }
