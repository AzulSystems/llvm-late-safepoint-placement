; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s

; CHECK: derived %next.i64 base %base_obj

define void @test(i64* %base_obj) {
entry:
  %obj = getelementptr i64* %base_obj, i32 1
  br label %loop

loop:
  %current = phi i64* [ %obj, %entry ], [ %next.i64, %loop ]
  %current.i32 = bitcast i64* %current to i32*
  %next.i32 = getelementptr i32* %current.i32, i32 1
  %next.i64 = bitcast i32* %next.i32 to i64*
  br label %loop
}
