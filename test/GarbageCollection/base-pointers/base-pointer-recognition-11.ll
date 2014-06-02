; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s

declare i64* @generate_obj()
declare void @use_obj(i64*)

define void @def_use_safepoint() {
entry:
  %obj = call i64* @generate_obj()
  br label %loop

loop:
; CHECK: %relocated = phi i64* [ %obj.relocated, %loop ], [ %obj, %entry ]
  call void @use_obj(i64* %obj)
  br label %loop
}
