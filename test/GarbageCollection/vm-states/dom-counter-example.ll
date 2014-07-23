;; RUN: opt -remove-redundant-vm-states %s -S | FileCheck %s

%jObject = type { [8 x i8] }

declare i32 @llvm.jvmstate_0()
declare i32 @llvm.jvmstate_1()
@llvm.jvmstate_anchor = private global i32 0

define void @nested_loop(i8* %ptr) {
entry:
; CHECK: entry:
; CHECK: jvmstate_0
  %a = call i32 @llvm.jvmstate_0()
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  br label %loop

loop:
; CHECK: loop:
; CHECK: jvmstate_1
  %b = call i32 @llvm.jvmstate_1()
  store volatile i32 %b, i32* @llvm.jvmstate_anchor
; This store can not be replayed and thus requires the vm_state inside the loop due to the backedge
  store volatile i8 0, i8* %ptr 
  br i1 undef, label %loop, label %exit

exit:
; CHECK: exit:
  ret void
}
