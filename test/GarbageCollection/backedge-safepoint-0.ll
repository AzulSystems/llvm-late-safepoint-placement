; RUN: llvm-link %s %p/Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-all-functions -S | FileCheck %s

; CHECK: %current = phi i64* [ %obj, %entry ], [ %next.relocated, %loop ]

define void @test(i64* %obj) {
entry:
  br label %loop

loop:
  %current = phi i64* [ %obj, %entry ], [ %next, %loop ]
  %next = getelementptr i64* %current, i32 1
  br label %loop
}