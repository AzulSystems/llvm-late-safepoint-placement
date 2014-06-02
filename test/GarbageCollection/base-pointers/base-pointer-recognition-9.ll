; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-all-functions -spp-print-base-pointers -S  2>&1 | FileCheck %s

; CHECK: derived %next base %base_obj

declare i1 @runtime_value()

define void @maybe_GEP(i64* %base_obj) {
entry:
  br label %loop

loop:
  %current = phi i64* [ %base_obj , %entry ], [ %next, %loop ]
  %condition = call i1 @runtime_value()

  %maybe_next = getelementptr i64* %current, i32 1
  %next = select i1 %condition, i64* %maybe_next, i64* %current
  br label %loop
}
