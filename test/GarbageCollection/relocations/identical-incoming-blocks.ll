; RUN: opt %s -place-safepoints

; ModuleID = 'bugpoint-input-916a0fc.bc'
target triple = "x86_64-unknown-linux-gnu"

%jObject = type { [8 x i8] }

@llvm.jvmstate_anchor = external global i32

; Function Attrs: noinline
define void @gc.safepoint_poll() #5 {
entry:
  tail call void @"Runtime::poll_at_safepoint_static"()
  ret void
}

declare void @"Runtime::poll_at_safepoint_static"()

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, i32, i32, %jObject addrspace(1)*, i32, %jObject addrspace(1)*, i32, i8*, i32, i8*, i32, i8*, i32, i8*, i32, i8*, i32, i8*, i32, i8*, i32, i8*) #6

; Function Attrs: nounwind
define void @"sun.net.www.MessageHeader::mergeHeader"(%jObject addrspace(1)*, %jObject addrspace(1)*) #7 {
bci_0:
  %2 = tail call i32 @llvm.jvmstate_0(i32 0, i32 0, i32 0, i32 10, i32 0, i32 12, %jObject addrspace(1)* %0, i32 12, %jObject addrspace(1)* %1, i32 99, i8* null, i32 99, i8* null, i32 99, i8* null, i32 99, i8* null, i32 99, i8* null, i32 99, i8* null, i32 99, i8* null, i32 99, i8* null)
  store volatile i32 %2, i32* @llvm.jvmstate_anchor
  %3 = tail call %jObject addrspace(1)* undef([1776 x i8]* undef, i32 5, i32 10) #8
  br label %in_bounds

in_bounds:                                        ; preds = %bci_0
  switch i32 undef, label %bci_209-iload [
    i32 10, label %bci_252-iload
    i32 13, label %bci_252-iload
  ]

bci_209-iload:                                    ; preds = %in_bounds
  br label %bci_252-iload

bci_252-iload:                                    ; preds = %in_bounds, %in_bounds, %bci_209-iload
  %local_2_181 = phi %jObject addrspace(1)* [ null, %bci_209-iload ], [ %3, %in_bounds ], [ %3, %in_bounds ]
  unreachable
}

attributes #5 = { noinline }
attributes #6 = { nounwind readonly "gc-leaf-function"="true" }
attributes #7 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
