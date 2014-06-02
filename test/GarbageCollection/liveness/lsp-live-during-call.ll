; RUN:  opt %s -place-call-safepoints -spp-print-liveset -spp-all-functions -S 2>&1 | FileCheck %s

; We're looking to make sure both objs show up as live appropriately even though
; there's no use after the call site
; CHECK: Live Variables:
; CHECK-NEXT: arg
; CHECK-NEXT: obj 

%jObject = type { [8 x i8] }

declare  void @"some_call"(%jObject addrspace(1)*, %jObject addrspace(1)*)

; Function Attrs: noreturn nounwind
define  void @"SimpleLobj::counted_loop"(%jObject addrspace(1)* %arg) #0 {
bci_0:
  %obj = load %jObject addrspace(1)** inttoptr (i64 1140842920 to %jObject addrspace(1)**), align 8
  tail call  void @"some_call"(%jObject addrspace(1)* %arg, %jObject addrspace(1)* %obj) #1
  ret void
}

attributes #0 = { noreturn nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true"}
attributes #1 = { nounwind }
