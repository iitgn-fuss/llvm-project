; REQUIRES: asserts
; RUN: rm -rf %t && split-file %s %t
; RUN: opt -passes=savio-ir -savio-force-count=1 -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -savio-port-info=%S/Inputs/port-info -savio-cpu=emeraldrapids -debug-only=savio -disable-output %t/narrow.ll 2>&1 | FileCheck %s --check-prefix=NARROW
; RUN: opt -passes=savio-ir -savio-force-count=1 -mtriple=x86_64-unknown-linux-gnu -mcpu=emeraldrapids -savio-port-info=%S/Inputs/port-info -savio-cpu=emeraldrapids -debug-only=savio -disable-output %t/wide.ll 2>&1 | FileCheck %s --check-prefix=WIDE
; Review Focus 3: with512 is region-wide. A loop whose vector ops are <8 x i32> and <4 x i32> has with512=0 in every
; estimate; the same loop plus one <16 x i32> op has with512=1 (every class then uses its ports_with_512).
; NARROW: with512=0
; NARROW-NOT: with512=1
; WIDE: with512=1
; WIDE-NOT: with512=0

;--- narrow.ll
define void @f(ptr noalias %k, ptr noalias %o, ptr noalias %p, ptr noalias %q, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pk = getelementptr inbounds i32, ptr %k, i64 %i
  %w = load <8 x i32>, ptr %pk, align 32
  %m = mul <8 x i32> %w, splat (i32 -2048144789)
  %s = lshr <8 x i32> %m, splat (i32 13)
  %x = xor <8 x i32> %s, %m
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store <8 x i32> %x, ptr %po, align 32
  %pp = getelementptr inbounds i32, ptr %p, i64 %i
  %v = load <4 x i32>, ptr %pp, align 16
  %a = add <4 x i32> %v, %v
  %pq = getelementptr inbounds i32, ptr %q, i64 %i
  store <4 x i32> %a, ptr %pq, align 16
  %i.next = add nuw i64 %i, 16
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

;--- wide.ll
define void @f(ptr noalias %k, ptr noalias %o, ptr noalias %p, ptr noalias %q, i64 %n) "prefer-vector-width"="512" {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pk = getelementptr inbounds i32, ptr %k, i64 %i
  %w = load <8 x i32>, ptr %pk, align 32
  %m = mul <8 x i32> %w, splat (i32 -2048144789)
  %s = lshr <8 x i32> %m, splat (i32 13)
  %x = xor <8 x i32> %s, %m
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store <8 x i32> %x, ptr %po, align 32
  %pp = getelementptr inbounds i32, ptr %p, i64 %i
  %v = load <4 x i32>, ptr %pp, align 16
  %a = add <4 x i32> %v, %v
  %pq = getelementptr inbounds i32, ptr %q, i64 %i
  store <4 x i32> %a, ptr %pq, align 16
  %z = add <16 x i32> zeroinitializer, zeroinitializer
  %pz = getelementptr inbounds i32, ptr %p, i64 %i
  %zz = add <16 x i32> %z, splat (i32 1)
  store <16 x i32> %zz, ptr %pz, align 64
  %i.next = add nuw i64 %i, 16
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
