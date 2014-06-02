; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s

; CHECK: derived %next_element_ptr base %array_obj

define i32 @null_in_array(i64* %array_obj) {
entry:
  %array_len_pointer.i64 = getelementptr i64* %array_obj, i32 1
  %array_len_pointer.i32 = bitcast i64* %array_len_pointer.i64 to i32*
  %array_len = load i32* %array_len_pointer.i32
  %array_elems = bitcast i32* %array_len_pointer.i32 to i64**
  br label %loop_check

loop_check:
  %index = phi i32 [ 0, %entry ], [ %next_index, %loop_back ]
  %current_element_ptr = phi i64** [ %array_elems, %entry ], [ %next_element_ptr, %loop_back ]
  %index_lt = icmp ult i32 %index, %array_len
  br i1 %index_lt, label %check_for_null, label %not_found

check_for_null:
  %current_element = load i64** %current_element_ptr
  %is_null = icmp eq i64* %current_element, null
  br i1 %is_null, label %found, label %loop_back

loop_back:
  %next_element_ptr = getelementptr i64** %current_element_ptr, i32 1
  %next_index = add i32 %index, 1
  br label %loop_check

not_found:
  ret i32 -1

found:
  ret i32 %index
}
