; RUN: opt %s -place-backedge-safepoints -verify-safepoint-ir -S 2>&1 | FileCheck %s

%jObject = type { [8 x i8] }

; Function Attrs: noinline
define void @gc.safepoint_poll() #0 {
entry:
  tail call  void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]* undef)
  ret void
}

declare  void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]*)

declare void @llvm.jvmstate_133(i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*)

; Function Attrs: nounwind
define  void @"spec.benchmarks.scimark.fft.FFT::transform_internal"([160 x i8]* nocapture readnone %method, %jObject  addrspace(1)*, %jObject addrspace(1)*, i32) #1 {
bci_0:
  %3 = icmp slt i32 undef, 2
  br label %bci_42-dconst_1

assert_failed:                                    ; preds = %bci_203-dload
  unreachable

bci_42-dconst_1:                                  ; preds = %bci_377-iinc, %bci_0
  br label %bci_203-dload

bci_203-dload:                                    ; preds = %bci_371-iinc, %bci_42-dconst_1
; CHECK: bci_371-iinc:                                     ; preds = %bci_203-dload
  tail call void @llvm.jvmstate_133(i32 203, i32 0, i32 2, i32 0, %jObject addrspace(1)* %0, %jObject addrspace(1)* %1)
; CHECK: statepoint
; CHECK: br
  br i1 %3, label %bci_371-iinc, label %assert_failed

bci_371-iinc:                                     ; preds = %bci_203-dload
  br i1 undef, label %bci_203-dload, label %bci_377-iinc

bci_377-iinc:                                     ; preds = %bci_371-iinc
  br label %bci_42-dconst_1
}

attributes #0 = { noinline }
attributes #1 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
