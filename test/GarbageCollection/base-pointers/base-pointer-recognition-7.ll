; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -spp-no-entry -spp-no-backedge -place-safepoints -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s

; CHECK: derived %merged_value base %base_phi

declare void @site_for_call_safpeoint()

define i64* @test(i64* %base_obj_x, i64* %base_obj_y, i1 %runtime_condition_x, i1 %runtime_condition_y) {
entry:
  br i1 %runtime_condition_x, label %here, label %there

here:
 br i1 %runtime_condition_y, label %bump_here_a, label %bump_here_b

bump_here_a:
  %x_a = getelementptr i64* %base_obj_x, i32 1
  br label %merge_here

bump_here_b:
  %x_b = getelementptr i64* %base_obj_y, i32 2
  br label %merge_here
  

merge_here:
; CHECK: merge_here:
; CHECK-DAG: %base_phi
; CHECK-DAG: phi i64*
; CHECK-DAG: [ %base_obj_x, %bump_here_a ]
; CHECK-DAG: [ %base_obj_y, %bump_here_b ]
  %x = phi i64* [ %x_a , %bump_here_a ], [ %x_b , %bump_here_b ]
  br label %merge

there:
  %y = getelementptr i64* %base_obj_y, i32 1
  br label %merge

merge:
; CHECK: merge:
; CHECK-DAG:  %base_phi1
; CHECK-DAG: phi i64*
; CHECK-DAG: %merge_here
; CHECK-DAG: [ %base_obj_y, %there ]
; CHECK:  %merged_value = phi i64* [ %x, %merge_here ], [ %y, %there ]  
  %merged_value = phi i64* [ %x, %merge_here ], [ %y, %there ]

  call void @site_for_call_safpeoint()
  ret i64* %merged_value
}
