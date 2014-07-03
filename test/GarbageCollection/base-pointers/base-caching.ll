; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-safepoints -spp-no-call -spp-no-entry -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s
;; The purpose of this test is to ensure that when two live values share a
;;  base defining value with inherent conflicts, we end up with a *single*
;;  base phi/select per such node.  This is testing an optimization, not a
;;  fundemental correctness criteria

define void @test(i64* %base_obj, i64* %base_arg2) {
entry:
  %obj = getelementptr i64* %base_obj, i32 1
  br label %loop

loop:
; CHECK-LABEL: loop
; CHECK:   %base_phi = phi i64* [ %base_obj, %entry ], [ %base_select.relocated, %loop ]
; CHECK-NOT: base_phi2

;; Both 'next' and 'extra2' are live across the backedge safepoint...
  %current = phi i64* [ %obj, %entry ], [ %next, %loop ]
  %extra = phi i64* [ %obj, %entry ], [ %extra2, %loop ]

  %nexta = getelementptr i64* %current, i32 1
  %next = select i1 undef, i64* %nexta, i64* %base_arg2
; CHECK: next = select
; CHECK: base_select
  %extra2 = select i1 undef, i64* %nexta, i64* %base_arg2
; CHECK: extra2 = select
; CHECK: base_select
; CHECK: statepoint
  br label %loop
}
