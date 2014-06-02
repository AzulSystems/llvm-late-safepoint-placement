; RUN: opt -place-backedge-safepoints -place-call-safepoints -place-entry-safepoints -verify -S < %s
; XFAIL: *

%jObject = type { [8 x i8] }
%jMethod = type { [160 x i8] }

@llvm.jvmstate_anchor = external global i32

declare void @do_safepoint()

define void @gc.safepoint_poll() {
entry:
  call void @do_safepoint()
  ret void
}

declare i32 @llvm.jvmstate_1(i32, i32, i32, i32) #7

declare i32 @llvm.jvmstate_9(i32, i32, i32, i32) #7

declare i32 @llvm.jvmstate_174(i32, i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, %jObject addrspace(1)*, i32, i32, i32, i32, %jObject addrspace(1)*) #7

;; Source: java.security.AccessControlContext::optimize

define %jObject addrspace(1)* @test([160 x i8]* nocapture readnone %method, %jObject addrspace(1)*) #10 {
bci_0:
  %1 = tail call i32 @llvm.jvmstate_1(i32 1, i32 0, i32 0, i32 0)
  store volatile i32 %1, i32* @llvm.jvmstate_anchor
  %2 = getelementptr inbounds %jObject addrspace(1)* %0, i32 0, i32 0, i32 8
  %3 = load i8 addrspace(1)* %2
  %4 = icmp eq i8 %3, 0
  br i1 %4, label %bci_15-invokestatic, label %bci_19-aload_0

bci_15-invokestatic:
  %compiled_entry = tail call i8* undef(%jMethod addrspace(1)* undef) #11
  %5 = bitcast i8* %compiled_entry to %jObject addrspace(1)* (%jMethod addrspace(1)*)*
  %6 = tail call %jObject addrspace(1)* %5(%jMethod addrspace(1)* undef) #11
  br label %bci_19-aload_0

bci_19-aload_0:
  %local_1_ = phi %jObject addrspace(1)* [ %6, %bci_15-invokestatic ], [ null, %bci_0 ]
  %7 = tail call i32 @llvm.jvmstate_9(i32 19, i32 0, i32 0, i32 0)
  store volatile i32 %7, i32* @llvm.jvmstate_anchor
  store volatile i32 undef, i32* @llvm.jvmstate_anchor
  %8 = icmp eq %jObject addrspace(1)* %local_1_, null
  br i1 %8, label %bci_70-iload_3, label %bci_53-aload_1

bci_53-aload_1:                                   ; preds = %bci_19-aload_0
  store volatile i32 undef, i32* @llvm.jvmstate_anchor
  br label %bci_70-iload_3

bci_70-iload_3:                                   ; preds = %bci_53-aload_1, %bci_19-aload_0
  %9 = tail call %jObject addrspace(1)* undef([1776 x i8]* undef, i32 50, i32 undef) #11
  %10 = tail call i32 @llvm.jvmstate_174(i32 314, i32 2, i32 7, i32 0, i32 0, %jObject addrspace(1)* %0, %jObject addrspace(1)* %0, %jObject addrspace(1)* %local_1_, i32 undef, i32 0, i32 0, i32 0, %jObject addrspace(1)* %9)
  store volatile i32 %10, i32* @llvm.jvmstate_anchor
  store i8 0, i8 addrspace(1)* %2
  ret %jObject addrspace(1)* %0
}

attributes #7 = { nounwind readonly "gc-leaf-function"="true" }
attributes #10 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
