; RUN: opt -passes=savio-ir -savio-port-info=%S/../../../../../src/port-info -savio-cpu=emeraldrapids -pass-remarks=savio -pass-remarks-missed=savio -debug-only=savio -disable-output %s 2>&1 | FileCheck %s
; RUN: opt -passes=savio-ir -savio-port-info=%S/../../../../../src/port-info -pass-remarks=savio -debug-only=savio -disable-output %S/unroll-adi-like.ll 2>&1 | FileCheck %s --check-prefix=OFF
; REQUIRES: asserts
; The charge is opt-in per port information (resources.json "charge_out_of_group":
; true, set for emeraldrapids only): a description without it (apple-m4 on the adi-like loop of unroll-adi-like.ll,
; the test fixtures) charges nothing outside the groups, as before (see the OFF checks).
; OFF: savio: class loads
; OFF-NOT: out-of-group
; Instructions outside every group (here the loads, the store and the induction
; add) are charged with their port-info cost, on emeraldrapids: they are present
; before and after a move, so they add a fixed load to the resource and dispatch
; bounds. The class loads line of the stock estimate lists the load and store
; classes next to the divider, and the integer divide itself charges its own port
; micro-ops (class scalar_mul = p1, class scalar_alu) on top of the divider time.
; CHECK: savio: out-of-group charge: {{.*}}load i32
; CHECK: savio: out-of-group charge: {{.*}}store i32
; CHECK: savio: class loads (cycles, whole region): int_div=6.000000e+00 load=2.000000e+00 scalar_alu={{[0-9.e+]+}} scalar_mul=2.790000e+00 store=1.000000e+00
target triple = "x86_64-unknown-linux-gnu"

define void @f(ptr %a, ptr %b, ptr %o, i64 %n) "target-cpu"="emeraldrapids" {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr inbounds i32, ptr %a, i64 %i
  %pb = getelementptr inbounds i32, ptr %b, i64 %i
  %x = load i32, ptr %pa, align 4
  %y = load i32, ptr %pb, align 4
  %d = or i32 %y, 1
  %q = udiv i32 %x, %d
  %po = getelementptr inbounds i32, ptr %o, i64 %i
  store i32 %q, ptr %po, align 4
  %i.next = add nuw nsw i64 %i, 1
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}
