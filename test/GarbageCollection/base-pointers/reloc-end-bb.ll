; RUN: opt %s -place-backedge-safepoints -place-call-safepoints  -spp-all-functions -S  2>&1 | FileCheck %s

; This case is testing for an edge case bug that caused a crash in
; findCurrentRelocationForBase.  The issue was that a safepoint
; inserted in a block with another safepoint and a computed phi
; above a previous relocation phi would crash due to not starting
; in the right place in the basic block.

%jObject = type { [8 x i8] }

; Function Attrs: noinline
define void @gc.safepoint_poll() #0 {
entry:
  tail call  void @"Runtime::poll_at_safepoint_static"([1768 x i8]* undef)
  ret void
}

declare  void @"Runtime::poll_at_safepoint_static"([1768 x i8]*)

; Function Attrs: nounwind
define  i32 @"spec.benchmarks.scimark.lu.LU::factor"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)* %arg, %jObject addrspace(1)* %arg1, %jObject addrspace(1)* %arg2) #2 {
bci_0:
  br label %not_zero50

not_zero50:                                       ; preds = %not_zero50, %bci_0
; CHECK: not_zero50
; CHECK: relocated
; This safepoint _has_ to be inserted after the backedge safepoint to trigger the bug
  %tmp4 = tail call double undef(double undef) #3
; There should not be use of arg1 in the safepoint for the call
; CHECK: statepoint
; CHECK: relocated
  %tmp5 = getelementptr inbounds %jObject addrspace(1)* %arg1, i64 undef
; CHECK: getelementptr
; CHECK: statepoint
  br label %not_zero50
}

attributes #0 = { noinline }
attributes #1 = { nounwind readonly "gc-leaf-function"="true" }
attributes #2 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
attributes #3 = { nounwind }
