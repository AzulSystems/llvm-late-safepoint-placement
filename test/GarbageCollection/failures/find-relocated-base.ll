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
  %frame_pointer = tail call i8* @llvm.frameaddress(i32 0)
  unreachable

safepointed:                                      ; preds = %entry
  ret void
}

; Function Attrs: nounwind readnone
declare  i8* @llvm.frameaddress(i32) #6

declare  void @"Runtime::poll_at_safepoint_static"([1776 x i8]*)

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_2(i32, i32, i32, i32, i32, %jObject addrspace(1)*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*) #7

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_71(i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, i32, i32, i32, i8*, i8*, i8*, i8*, i8*) #7

declare  %jObject addrspace(1)* @"Runtime::anewarray"([1776 x i8]*, i32, i32) #9

; Function Attrs: nounwind
define  %jObject addrspace(1)* @"java.security.AccessControlContext::optimize"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)*) #10 {
bci_0:
  br label %assert_ok4

assert_ok4:                                       ; preds = %bci_0
  %1 = tail call i32 @llvm.jvmstate_2(i32 4, i32 1, i32 10, i32 0, i32 undef, %jObject addrspace(1)* %0, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null)
  %2 = tail call  %jObject addrspace(1)* undef(%jMethod addrspace(1)* undef) #5
  br label %bci_19-aload_0

assert_ok8:                                       ; No predecessors!
  br label %bci_19-aload_0

bci_19-aload_0:                                   ; preds = %assert_ok8, %assert_ok4
  %3 = getelementptr inbounds %jObject addrspace(1)* %0, i64 0, i32 0, i64 16
  %addr21 = bitcast i8 addrspace(1)* %3 to %jObject addrspace(1)* addrspace(1)*
  br label %bci_43-iconst_1

assert_ok35:                                      ; No predecessors!
  br label %bci_43-iconst_1

bci_43-iconst_1:                                  ; preds = %assert_ok35, %bci_19-aload_0
  br i1 undef, label %bci_70-iload_3, label %bci_53-aload_1

bci_53-aload_1:                                   ; preds = %bci_43-iconst_1
  br label %bci_70-iload_3

bci_70-iload_3:                                   ; preds = %bci_53-aload_1, %bci_43-iconst_1
  br label %not_zero100

not_zero100:                                      ; preds = %bci_70-iload_3
  br label %not_zero119

not_zero119:                                      ; preds = %not_zero100
  %4 = tail call i32 @llvm.jvmstate_71(i32 128, i32 0, i32 10, i32 0, %jObject addrspace(1)* %0, %jObject addrspace(1)* undef, i32 undef, i32 1, i32 undef, i8* null, i8* null, i8* null, i8* null, i8* null)
  %5 = tail call  %jObject addrspace(1)* @"Runtime::anewarray"([1776 x i8]* undef, i32 50, i32 undef) #5
  %obj.1296 = load %jObject addrspace(1)* addrspace(1)* %addr21, align 8
  unreachable
}

attributes #0 = { alwaysinline nounwind readnone "gc-leaf-function"="true" }
attributes #1 = { noinline nounwind "gc-leaf-function"="true" }
attributes #2 = { alwaysinline nounwind readonly "gc-leaf-function"="true" }
attributes #3 = { noinline nounwind readnone "gc-leaf-function"="true" }
attributes #4 = { noinline }
attributes #5 = { nounwind }
attributes #6 = { nounwind readnone }
attributes #7 = { nounwind readonly "gc-leaf-function"="true" }
attributes #8 = { "gc-leaf-function"="true" }
attributes #9 = { "calloc_like"="true" }
attributes #10 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }

!0 = metadata !{metadata !"branch_weights", i32 4, i32 64}
