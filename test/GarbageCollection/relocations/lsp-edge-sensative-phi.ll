; RUN: opt %s -place-call-safepoints -spp-all-functions -S 2>&1 | FileCheck %s

; The case we're checking for is that a safepoint insert after another one doesn't
; introduce a use of a value that we already relocated.  This can happen if you take
; a slightly mismatched definition of reachability in the updating vs liveness checks

%jObject = type { [8 x i8] }

declare  void @"some_call"(%jObject addrspace(1)*) #1

; Function Attrs: noreturn nounwind
define  void @"edge_senative_reachability"(%jObject addrspace(1)* %arg) #0 {
bci_0:
  br label %bci_8-aload_0

bci_8-aload_0:                                    ; preds = %bci_8-aload_0, %bci_0
  ; CHECK: statepoint 
  ; CHECK: arg.relocated
  tail call  void @"some_call"(%jObject addrspace(1)* %arg) #1
  ; CHECK: statepoint 
  tail call  void @"some_call"(%jObject addrspace(1)* %arg) #1
  ; CHECK-NOT: arg.relocated2
  ; CHECK: arg.relocated.relocated
  br label %bci_8-aload_0
}

attributes #0 = { noreturn nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
attributes #1 = { nounwind }
