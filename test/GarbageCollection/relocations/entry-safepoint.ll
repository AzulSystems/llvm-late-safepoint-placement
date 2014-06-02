; RUN: opt -S -place-backedge-safepoints -place-entry-safepoints %s

%jObject = type { [8 x i8] }

define void @gc.safepoint_poll() #0 {
entry:
  br i1 undef, label %safepointed, label %do_safepoint

do_safepoint:
  call void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]* undef)
  br label %safepointed

safepointed:
  ret void
}

declare void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]*)

declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, %jObject addrspace(1)*) #1

;; Source "com.sun.crypto.provider.AESCrypt::encryptBlock"

define void @test(%jObject addrspace(1)*, %jObject addrspace(1)*, %jObject addrspace(1)*) #2 {
entry:
  call i32 @llvm.jvmstate_0(i32 0, i32 0, i32 3, i32 0, %jObject addrspace(1)* %0, %jObject addrspace(1)* %1, %jObject addrspace(1)* %2)
  br label %loop

loop:
  call void undef(%jObject addrspace(1)* %0, %jObject addrspace(1)* %1, %jObject addrspace(1)* %2)
  br label %loop
}

attributes #1 = { nounwind readonly "gc-leaf-function"="true" }
attributes #2 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
