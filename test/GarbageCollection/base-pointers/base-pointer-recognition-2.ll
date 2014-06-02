; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-call-safepoints -spp-print-base-pointers -spp-all-functions -S 2>&1 | FileCheck %s

; CHECK: derived %merged_value base %base_obj

declare void @site_for_call_safpeoint()

define i64* @test(i64* %base_obj, i1 %runtime_condition) {
entry:
  br i1 %runtime_condition, label %merge, label %there

there:
  %derived_obj = getelementptr i64* %base_obj, i32 1
  br label %merge

merge:
  %merged_value = phi i64* [ %base_obj, %entry ], [ %derived_obj, %there ]
  call void @site_for_call_safpeoint()
  ret i64* %merged_value
}
