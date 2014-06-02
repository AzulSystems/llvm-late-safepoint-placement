; RUN: opt %s  -place-backedge-safepoints -place-call-safepoints -S 2>&1 | FileCheck %s

; This test is basically ensuring that having a relocation encounter a new safepoint
; works correctly.  We currently stop before this can happen, but an alternate approach
; would be to treat relocations as kills and update the propagated value correctly.

; TODO: this test could be simplified a bit

%jObject = type { [8 x i8] }

; Function Attrs: noinline
define void @gc.safepoint_poll() #1 {
entry:
  br i1 undef, label %safepointed, label %do_safepoint

do_safepoint:                                     ; preds = %entry
  tail call  void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]* undef)
  br label %safepointed

safepointed:                                      ; preds = %do_safepoint, %entry
  ret void
}

declare  void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]*)

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_16(i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, i32, %jObject addrspace(1)*, i32, %jObject addrspace(1)*, i32, i32, i8*, i8*, i8*, i8*, i8*, i8*, i8*) #2

declare  %jObject addrspace(1)* @"VMRuntime::newarray"([1768 x i8]*, i32, i32) #3

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_36(i32, i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, i32, %jObject addrspace(1)*, i32, %jObject addrspace(1)*, i32, i32, i64, i8*, i32, i32, i8*, i8*, i8*) #2

; Function Attrs: nounwind
define  %jObject addrspace(1)* @"java.math.BigInteger::multiplyToLen"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)* readonly %arg, %jObject addrspace(1)* %arg1, i32 %arg2, %jObject addrspace(1)* %arg3, i32 %arg4, %jObject addrspace(1)* %arg5) #4 {
bci_0:
  %tmp = tail call i32 @llvm.jvmstate_16(i32 26, i32 0, i32 15, i32 0, %jObject addrspace(1)* %arg, %jObject addrspace(1)* %arg1, i32 %arg2, %jObject addrspace(1)* %arg3, i32 %arg4, %jObject addrspace(1)* %arg5, i32 undef, i32 undef, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null)
  %tmp6 = tail call  %jObject addrspace(1)* @"VMRuntime::newarray"([1768 x i8]* undef, i32 10, i32 undef) #5
  br label %not_zero48

not_zero48:                                       ; preds = %not_zero48, %bci_0
  %tmp7 = tail call i32 @llvm.jvmstate_36(i32 59, i32 1, i32 15, i32 0, i32 undef, %jObject addrspace(1)* %arg, %jObject addrspace(1)* %arg1, i32 %arg2, %jObject addrspace(1)* %arg3, i32 %arg4, %jObject addrspace(1)* %tmp6, i32 undef, i32 undef, i64 undef, i8* null, i32 undef, i32 undef, i8* null, i8* null, i8* null)
; CHECK: poll.exit
; CHECK: not_zero48
  br label %not_zero48
}

attributes #0 = { noinline nounwind readnone  "gc-leaf-function"="true" }
attributes #1 = { noinline  }
attributes #2 = { nounwind readonly "gc-leaf-function"="true" }
attributes #3 = { "calloc_like"="true" }
attributes #4 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true"  }
attributes #5 = { nounwind }
