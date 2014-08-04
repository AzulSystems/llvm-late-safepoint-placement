; RUN: opt -safepoint-ir-verifier-print-only -verify-safepoint-ir -S %s 2>&1 | FileCheck %s

; This test is very simple.  It just checks that if a value is
; used immediately after a safepoint without using the relocated
; value that the verifier catches this.  

; We previously had a bug where we'd early exit from a block before
; ever processing it.  The InvalidOnEntry set matched the stored one
; (i.e. the empty set), and we'd never compute a meaningful 
; InvalidOnExit.  This test catches that and other errors

%jObject = type { [8 x i8] }

; Function Attrs: nounwind
define %jObject addrspace(1)* @test(%jObject addrspace(1)* %arg) {
bci_0:
  %safepoint_token3 = tail call i32 (double (double)*, i32, i32, i32, i32, i32, i32, i32, ...)* @llvm.statepoint.p0f_f64f64f(double (double)* undef, i32 1, i32 0, i32 0, i32 -1, i32 0, i32 0, i32 0, double undef, %jObject addrspace(1)* %arg)
  %arg2.relocated4 = call coldcc %jObject addrspace(1)* @llvm.gc.relocate.p1jObject(i32 %safepoint_token3, i32 9, i32 9)
  ret %jObject addrspace(1)* %arg
; CHECK: Illegal use of unrelocated value after safepoint found!
; CHECK-NEXT: Def: %jObject addrspace(1)* %arg
; CHECK-NEXT: Use:   ret %jObject addrspace(1)* %arg
}

; Function Attrs: nounwind
declare %jObject addrspace(1)* @llvm.gc.relocate.p1jObject(i32, i32, i32) #3

declare i32 @llvm.statepoint.p0f_f64f64f(double (double)*, i32, i32, i32, i32, i32, i32, i32, ...)
