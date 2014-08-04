;; RUN: opt -S -remove-redundant-vm-states < %s | FileCheck %s
;; Note: -improve-vmstate-placement would fail this test.  That
;; is, in fact, the motivation to moving to -remove-redundant-vm-states

declare i32 @llvm.jvmstate_1()

declare i32 @llvm.jvmstate_2()

declare i32 @llvm.jvmstate_3()

@llvm.jvmstate_anchor = private global i32 0
@other_effect = private global i32 0

define void @nested_loop() {
entry:
  %a = call i32 @llvm.jvmstate_1()
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  br label %outer_loop

outer_loop:
  %b = call i32 @llvm.jvmstate_2()
  store volatile i32 %b, i32* @llvm.jvmstate_anchor
  br label %inner_loop

inner_loop:
  %c = call i32 @llvm.jvmstate_3()
  store volatile i32 %c, i32* @llvm.jvmstate_anchor
  br i1 undef, label %inner_loop.exit, label %inner_loop

inner_loop.exit:
  store i32 undef, i32* undef
  br label %outer_loop
}

; CHECK: %a = call i32 @llvm.jvmstate_1()
; CHECK: %b = call i32 @llvm.jvmstate_2()
; CHECK-NOT: %c = call i32 @llvm.jvmstate_3()
