;; RUN: opt -S -remove-redundant-vm-states < %s | FileCheck %s

;; This test verifies that we do not remove any vmstate@callsite
;; as a redundant vmstate. A vmstate@callsite is directly linked 
;; to the CallInstr, and any vmstate that dominate it could not 
;; replace it and mark it as redundant.
 
declare i32 @llvm.jvmstate_1()

declare i32 @llvm.jvmstate_2()

@llvm.jvmstate_anchor = private global i32 0

declare void @foo(i32 %callVMSHolder)

define void @simple() {
entry:
  %a = call i32 @llvm.jvmstate_1()
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  br label %next.0

next.0:
  %b = call i32 @llvm.jvmstate_2()
  call void @foo(i32 %b)
  ret void
}

; CHECK: call i32 @llvm.jvmstate_2()

