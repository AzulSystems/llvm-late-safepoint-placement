; RUN:  opt %s -place-call-safepoints -spp-print-liveset -spp-all-functions -S 2>&1 | FileCheck %s
target triple = "x86_64-unknown-linux-gnu"

%jObject = type { [8 x i8] }

declare  void @some_call(i64)

; Function Attrs: nounwind readonly
declare i32 @some_use(%jObject addrspace(1)*) #0

; Function Attrs: nounwind
define  void @"spec.benchmarks.scimark.fft.FFT::transform_internal"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)* %arg, %jObject addrspace(1)* %arg1, i32 %arg2) #1 {

bci_0:
  tail call void @some_call(i64 undef)
; CHECK: statepoint
  %"4" = tail call i32 @some_use(%jObject addrspace(1)* %arg)
  tail call void @some_call(i64 undef)
; CHECK: statepoint
  br label %bci_389-return

bci_377-iinc:                                     ; No predecessors!
  br label %bci_389-return

bci_389-return:                                   ; preds = %bci_377-iinc, %bci_0
  ret void
}

attributes #0 = { nounwind readonly "gc-leaf-function"="true" }
attributes #1 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true"}
