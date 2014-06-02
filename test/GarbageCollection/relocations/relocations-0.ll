; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-all-functions -spp-print-base-pointers -S 2>&1 | FileCheck %s

declare i64* @generate_obj()
declare void @consume_obj(i64*)
declare i1 @rt()

define void @test() {
entry:
  %obj_init = call i64* @generate_obj()
  %obj = getelementptr i64* %obj_init, i32 42
  br label %loop

loop:
; CHECK: loop:
; CHECK-NEXT: %relocated2 = phi i64* [ %obj_init.relocated, %loop.backedge ], [ %obj_init, %entry ]
; CHECK-NEXT: %relocated = phi i64* [ %obj.relocated, %loop.backedge ], [ %obj, %entry ]
  %index = phi i32 [ 0, %entry], [ %index.inc, %loop_x ], [ %index.inc, %loop_y ]
; CHECK: %location = getelementptr i64* %relocated, i32 %index
  %location = getelementptr i64* %obj, i32 %index
  call void @consume_obj(i64* %location)
  %index.inc = add i32 %index, 1
  %condition = call i1 @rt()
  br i1 %condition, label %loop_x, label %loop_y

loop_x:
  br label %loop

loop_y:
  br label %loop
}