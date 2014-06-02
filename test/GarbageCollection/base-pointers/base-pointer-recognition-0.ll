; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s

; CHECK: derived %next base %base_obj

define void @test(i64* %base_obj) {
entry:
  %obj = getelementptr i64* %base_obj, i32 1
  br label %loop

loop:
; CHECK:   %relocated = phi i64* [ %base_obj.relocated, %loop ], [ %base_obj, %entry ]
; CHECK-NEXT:  %current = phi i64* [ %obj, %entry ], [ %next.relocated, %loop ]

  %current = phi i64* [ %obj, %entry ], [ %next, %loop ]
  %next = getelementptr i64* %current, i32 1
  br label %loop
}