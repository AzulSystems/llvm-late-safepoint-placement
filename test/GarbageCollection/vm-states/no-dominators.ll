;; RUN: opt -S -remove-redundant-vm-states < %s | FileCheck %s

declare i32 @llvm.jvmstate_1()

declare i32 @llvm.jvmstate_2()

declare i32 @llvm.jvmstate_3()

@llvm.jvmstate_anchor = private global i32 0
@other_effect = private global i32 0

define void @no_dominators() {
entry:
  br i1 undef, label %left, label %right

left:
  %a = call i32 @llvm.jvmstate_1()
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  br label %merge

right:
  %b = call i32 @llvm.jvmstate_2()
  store volatile i32 %b, i32* @llvm.jvmstate_anchor
  br label %merge

merge:
  %c = call i32 @llvm.jvmstate_3()
  store volatile i32 %c, i32* @llvm.jvmstate_anchor
  ret void
}

; CHECK: %a = call i32 @llvm.jvmstate_1()
; CHECK: %b = call i32 @llvm.jvmstate_2()
; CHECK: %c = call i32 @llvm.jvmstate_3()
