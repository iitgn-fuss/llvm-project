; RUN: opt -passes=savio-ir -savio-port-info=%S/../../../../../src/port-info -pass-remarks=savio -pass-remarks-missed=savio -S %s 2>&1 | FileCheck %s
; Data-dependent accesses on emeraldrapids (resources.json "indirect_access": 5.18
; cycles per gather, 8.23 per scatter, measured at a 4 MiB footprint; indices that
; provably stay under 48 KiB are not charged). The divide alone is bound at 6.00 by
; the integer divider, so moving a share to the vector unit would lower T; the memory
; floor of the access is above it and nothing can be gained.
; The index comes from a load: the scatter's 8.23 is the floor, the loop is declined.
; CHECK: remark: {{.*}}: T 8.23 (memory)
; The same loop with a masked index (256 words = 1 KiB, resident): the floor is gone,
; the divide is moved as without the access (the remarks come in function order:
; @scatter, @scatter_masked, @gather).
; CHECK: remark: {{.*}}: T 6.00 -> 2.40 (resource), 3/5 of slice
; A gather (load by a loaded index) feeding the divide: 5.18 is below the divider's 6.00,
; so the floor does not limit and the divide is moved down to the floor.
; CHECK: remark: {{.*}}: T 6.00 -> 5.18 (resource), 1/1 of slice
target triple = "x86_64-unknown-linux-gnu"

define void @scatter(ptr %a, ptr %idx, ptr %o, i32 %d, i64 %n) "target-cpu"="emeraldrapids" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %pi = getelementptr inbounds i32, ptr %idx, i64 %i
  %x = load i32, ptr %pa
  %j = load i32, ptr %pi
  %q = udiv i32 %x, %d
  %jz = zext i32 %j to i64
  %po = getelementptr inbounds i32, ptr %o, i64 %jz
  store i32 %q, ptr %po
  %i.next = add nuw i64 %i, 1
  %c = icmp eq i64 %i.next, %n
  br i1 %c, label %exit, label %loop

exit:
  ret void
}

define void @scatter_masked(ptr %a, ptr %idx, ptr %o, i32 %d, i64 %n) "target-cpu"="emeraldrapids" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %pi = getelementptr inbounds i32, ptr %idx, i64 %i
  %x = load i32, ptr %pa
  %j = load i32, ptr %pi
  %jm = and i32 %j, 255
  %q = udiv i32 %x, %d
  %jz = zext i32 %jm to i64
  %po = getelementptr inbounds i32, ptr %o, i64 %jz
  store i32 %q, ptr %po
  %i.next = add nuw i64 %i, 1
  %c = icmp eq i64 %i.next, %n
  br i1 %c, label %exit, label %loop

exit:
  ret void
}

define void @gather(ptr %a, ptr %idx, ptr %o, i32 %d, i64 %n) "target-cpu"="emeraldrapids" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pi = getelementptr inbounds i32, ptr %idx, i64 %i
  %j = load i32, ptr %pi
  %jz = zext i32 %j to i64
  %pa = getelementptr inbounds i32, ptr %a, i64 %jz
  %x = load i32, ptr %pa
  %q = udiv i32 %x, %d
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store i32 %q, ptr %po
  %i.next = add nuw i64 %i, 1
  %c = icmp eq i64 %i.next, %n
  br i1 %c, label %exit, label %loop

exit:
  ret void
}
