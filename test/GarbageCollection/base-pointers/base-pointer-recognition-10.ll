; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-print-base-pointers -spp-all-functions -S 2>&1 | FileCheck %s

; CHECK: derived %next_x base %base_obj_x
; CHECK: derived %next_y base %base_obj_y
; CHECK: derived %next base %base_phi

declare i1 @runtime_value()

define void @select_of_phi(i64* %base_obj_x, i64* %base_obj_y) {
entry:
  br label %loop

loop:
  %current_x = phi i64* [ %base_obj_x , %entry ], [ %next_x, %merge ]
  %current_y = phi i64* [ %base_obj_y , %entry ], [ %next_y, %merge ]
  %current = phi i64* [ null , %entry ], [ %next , %merge ]

  %condition = call i1 @runtime_value()
  %next_x = getelementptr i64* %current_x, i32 1
  %next_y = getelementptr i64* %current_y, i32 1

  br i1 %condition, label %true, label %false

true:
  br label %merge

false:
  br label %merge

merge:
  %next = phi i64* [ %next_x, %true ], [ %next_y, %false ]
  br label %loop
}
