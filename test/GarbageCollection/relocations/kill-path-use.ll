; RUN: opt -S %s -place-call-safepoints -place-entry-safepoints -spp-all-functions  2>&1 
; TODO: add this once it doesn't crash | FileCheck %s

%jObject = type { [8 x i8] }

define void @gc.safepoint_poll() #0 {
entry:
  tail call  void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]* undef)
  ret void
}
declare  void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]*)

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_2(i32, i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, %jObject addrspace(1)*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*) #1

; Function Attrs: nounwind
define  i32 @"spec.benchmarks.scimark.lu.LU::factor"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)* %arg, %jObject addrspace(1)* %arg1, %jObject addrspace(1)* %arg2) #2 {
bci_0:
  %array_begin = getelementptr inbounds %jObject addrspace(1)* %arg1, i64 0, i32 0, i64 64
  %tmp3 = bitcast i8 addrspace(1)* %array_begin to %jObject addrspace(1)* addrspace(1)*
  br label %not_zero50

not_zero50:                                       ; preds = %bci_241-aload, %bci_0
; The problem is that we get a relocation phi placed here by the entry safepoint which has an invalid obj 'entry_begin' coming in along it's other path (because of the kill in this BB, but going through the next one.)
  %array_begin_phi = phi i8 addrspace(1)* [ %array_begin2, %bci_241-aload ], [ %array_begin, %bci_0 ]
  %tmp4 = tail call double undef(double undef)
  %array_begin2 = getelementptr inbounds %jObject addrspace(1)* %arg2, i64 0, i32 0, i64 16
  br label %bci_241-aload

bci_241-aload:                                    ; preds = %not_zero50
  br label %not_zero50
}

attributes #0 = { noinline  }
attributes #1 = { nounwind readonly "gc-leaf-function"="true" }
attributes #2 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
