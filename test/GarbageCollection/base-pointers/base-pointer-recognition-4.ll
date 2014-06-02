; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-call-safepoints -place-backedge-safepoints -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s

; CHECK: derived %obj_to_consume base %base_phi

declare i64* @generate_obj()
declare void @consume_obj(i64*)

define void @test(i32 %condition) {
entry:
  br label %loop

loop:
; CHECK: loop:
; CHECK-NEXT:  %safepoint_token = call i32 (i64* ()*, i32, i32, i32, i32, i32, i32, ...)* @llvm.statepoint.p0f_p0i64f(i64* ()* @generate_obj, i32 0, i32 0, i32 -1, i32 0, i32 0, i3
; CHECK-NEXT:  %obj1 = call i64* @llvm.gc.result.ptr.p0i64(i32 %safepoint_token)
  %obj = call i64* @generate_obj()
  switch i32 %condition, label %dest_a [ i32 0, label %dest_b 
  	     		       	   i32 1, label %dest_c ]

dest_a:
  br label %merge;
dest_b:
  br label %merge;
dest_c:
  br label %merge;

merge:
; CHECK: merge:
; CHECK:  %base_phi = phi i64* [ %obj1, %dest_a ], [ null, %dest_b ], [ null, %dest_c ]
; CHECK:  %obj_to_consume = phi i64* [ %obj1, %dest_a ], [ null, %dest_b ], [ null, %dest_c ]

  %obj_to_consume = phi i64* [ %obj, %dest_a ] , [ null, %dest_b ], [ null, %dest_c ]
  call void @consume_obj(i64* %obj_to_consume)
  br label %loop
}
