; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-call-safepoints -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s

; CHECK: derived %merged_value base %base_phi

declare void @site_for_call_safpeoint()

define i64* @test(i64* %base_obj_x, i64* %base_obj_y, i1 %runtime_condition) {
entry:
  br i1 %runtime_condition, label %here, label %there

here:
 br label %bump

bump:
 br label %merge

there:
  %y = getelementptr i64* %base_obj_y, i32 1
  br label %merge

merge:
; CHECK: merge:
; CHECK-NEXT:  %base_phi = phi i64* [ %base_obj_x, %bump ], [ %base_obj_y, %there ]
; CHECK-NEXT:  %merged_value = phi i64* [ %base_obj_x, %bump ], [ %y, %there ]  
  %merged_value = phi i64* [ %base_obj_x, %bump ], [ %y, %there ]

  call void @site_for_call_safpeoint()
  ret i64* %merged_value
}
