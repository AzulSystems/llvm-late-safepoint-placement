;; RUN: opt -S -remove-redundant-vm-states < %s | FileCheck %s

declare i32 @llvm.jvmstate_1()

declare i32 @llvm.jvmstate_2()

declare i32 @llvm.jvmstate_3()

@llvm.jvmstate_anchor = private global i32 0

define void @simple() {
entry:
  %a = call i32 @llvm.jvmstate_1()
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  br label %next.0

next.0:
  %b = call i32 @llvm.jvmstate_2()
  store volatile i32 %b, i32* @llvm.jvmstate_anchor
  br label %next.1

next.1:
  %c = call i32 @llvm.jvmstate_3()
  store volatile i32 %c, i32* @llvm.jvmstate_anchor
  ret void
}

; CHECK-NOT: call i32 @llvm.jvmstate_2()
; CHECK-NOT: call i32 @llvm.jvmstate_3()
