;; RUN: opt -remove-redundant-vm-states %s -S | FileCheck %s

%jObject = type { [8 x i8] }

declare i32 @llvm.jvmstate_0()
declare i32 @llvm.jvmstate_1()
declare i32 @llvm.jvmstate_1b()
declare i32 @llvm.jvmstate_2()
@llvm.jvmstate_anchor = private global i32 0

define void @nested_loop(i8* %ptr) {
entry:
; CHECK: entry:
; CHECK: jvmstate_0
  %a = call i32 @llvm.jvmstate_0()
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  br i1 undef, label %loop, label %loop2

loop2:
; CHECK: loop2:
; CHECK: jvmstate_1
  %b = call i32 @llvm.jvmstate_1()
  store volatile i32 %b, i32* @llvm.jvmstate_anchor
; This one will need to be here since it's reachable with two different states
  br label %next

next:
; CHECK-NOT: jvmstate_1b
  %c = call i32 @llvm.jvmstate_1b()
  store volatile i32 %c, i32* @llvm.jvmstate_anchor
; We should be able to remove this vm state use, but that requires that we stablize
; the input state of this block not as 'conflict'
  br label %loop2


loop:
; CHECK: loop:
; CHECK: jvmstate_2
  %d = call i32 @llvm.jvmstate_2()
  store volatile i32 %d, i32* @llvm.jvmstate_anchor
; This store can not be replayed and thus requires the vm_state inside the loop due to the backedge
  store volatile i8 0, i8* %ptr 
  br i1 undef, label %loop, label %loop2

}
