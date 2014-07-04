; RUN: opt %s -place-safepoints  -verify -S | FileCheck %s

%jObject = type { [8 x i8] }

declare i32 @llvm.jvmstate_2(i32, i32, i32, i32, i32, %jObject addrspace(1)*)

declare void @foo()

define  %jObject addrspace(1)* @test(%jObject addrspace(1)*) #0 {
bci_0:
  %1 = tail call i32 @llvm.jvmstate_2(i32 0, i32 1, i32 0, i32 0, i32 0, %jObject addrspace(1)* %0)
; %0 must be live and relocated by this safepoint, even
; though it's not visibly used in the original IR
  tail call void @foo()
; CHECK: statepoint
; CHECK: gc.relocate
; This second safepoint needs the VM state information and
; thus it needs to be live until this call
  tail call void @foo()
; CHECK: statepoint
; CHECK: gc.relocate
  ret %jObject addrspace(1)* null
}

attributes #0 = { nounwind "gc-add-call-safepoints"="true"}
