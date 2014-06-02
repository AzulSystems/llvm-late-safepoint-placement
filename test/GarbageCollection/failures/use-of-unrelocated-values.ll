; RUN: opt %s -place-entry-safepoints -place-backedge-safepoints -place-call-safepoints -verify -S
; XFAIL: *

%jNotAtSP = type { [8 x i8] }
%jObject = type { [8 x i8] }
%jMethod = type { [160 x i8] }

@llvm.jvmstate_anchor = private global i32 0

; Function Attrs: noinline
define void @gc.safepoint_poll() #4 {
entry:
  br i1 undef, label %safepointed, label %do_safepoint

do_safepoint:                                     ; preds = %entry
  %stack_pointer = tail call i8* @llvm.stacksave()
  unreachable

safepointed:                                      ; preds = %entry
  ret void
}

; Function Attrs: nounwind
declare  i8* @llvm.stacksave() #5

declare  void @"Runtime::poll_at_safepoint_static"([1768 x i8]*)

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, %jObject addrspace(1)*, i8*, i8*, i8*) #7

declare  i1 @"Runtime::verify_obj"(%jObject addrspace(1)*) #8

declare  void @"Runtime::verify_code_cache_pc"(i64)

declare  %jObject addrspace(1)* @"Runtime::newarray"([1768 x i8]*, i32, i32) #9

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_67(i32, i32, i32, i32, %jObject addrspace(1)*, i32, i32, i8*) #7

; Function Attrs: nounwind
define  void @"java.io.BufferedReader::fill"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)*) #10 {
bci_0:
  %1 = tail call i32 @llvm.jvmstate_0(i32 0, i32 0, i32 4, i32 0, %jObject addrspace(1)* %0, i8* null, i8* null, i8* null)
  %2 = getelementptr inbounds %jObject addrspace(1)* %0, i64 0, i32 0, i64 56
  %addr38 = bitcast i8 addrspace(1)* %2 to %jObject addrspace(1)* addrspace(1)*
  br i1 undef, label %zero, label %not_zero, !prof !0

zero:                                             ; preds = %bci_0
  ret void

not_zero:                                         ; preds = %bci_0
  br i1 undef, label %bci_86-aload_0, label %bci_59-aload_0

bci_59-aload_0:                                   ; preds = %not_zero
  tail call void @"Runtime::verify_code_cache_pc"(i64 undef) #5
  br label %bci_119-aload_0

bci_86-aload_0:                                   ; preds = %not_zero
  %3 = tail call i1 @"Runtime::verify_obj"(%jObject addrspace(1)* undef) #5
  tail call void @"Runtime::verify_code_cache_pc"(i64 undef) #5
  %addr = bitcast %jObject addrspace(1)* addrspace(1)* %addr38 to %jNotAtSP addrspace(1)* addrspace(1)*
  br label %bci_119-aload_0

bci_119-aload_0:                                  ; preds = %bci_86-aload_0, %bci_59-aload_0
  %4 = tail call i32 @llvm.jvmstate_67(i32 119, i32 0, i32 4, i32 0, %jObject addrspace(1)* %0, i32 undef, i32 undef, i8* null)
  unreachable
}

attributes #4 = { noinline }
attributes #5 = { nounwind }
attributes #7 = { nounwind readonly "gc-leaf-function"="true" }
attributes #8 = { "gc-leaf-function"="true" }
attributes #9 = { "calloc_like"="true" }
attributes #10 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }

!0 = metadata !{metadata !"branch_weights", i32 4, i32 64}
