;; RUN: opt -S -remove-redundant-vm-states < %s | FileCheck %s

declare i32 @llvm.jvmstate_1()

declare i32 @llvm.jvmstate_2()

@llvm.jvmstate_anchor = private global i32 0

define void @simple() {
entry:
  %a = call i32 @llvm.jvmstate_1()
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  br label %loop

loop:
  %b = call i32 @llvm.jvmstate_2()
  store volatile i32 %b, i32* @llvm.jvmstate_anchor
  br i1 undef, label %loop, label %loop.exit

loop.exit:
  ret void
}

; CHECK-NOT: %b = call i32 @llvm.jvmstate_2()
