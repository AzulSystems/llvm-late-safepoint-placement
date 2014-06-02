; RUN: opt %s  -place-backedge-safepoints -place-call-safepoints -spp-all-functions -S 2>&1 | FileCheck %s

; This is testing a corner case in the relocation algorithm.  It is actually legal to
; encounter a kill (safepoint) before a newly inserted relocation phi.

%jObject = type { [8 x i8] }

; Function Attrs: noinline
define void @gc.safepoint_poll() #0 {
entry:
  tail call  void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]* undef)
  ret void
}

declare  void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]*)

; Function Attrs: nounwind
define  void @"sun.security.provider.SHA::implCompress"(%jObject addrspace(1)* %arg1) #2 {
bci_0:
; The live set here
  tail call  void undef(%jObject addrspace(1)* %arg1)
  br label %not_zero625.us

not_zero625.us:                                   ; preds = %not_zero625.us, %bci_0
; is larger than the one here (backedge safepoint)
; we can insert a relocation phi here, but there can't be any uses of it
; CHECK: %relocated = phi %jObject addrspace(1)* [ null, %not_zero625.us ], [ %arg1.relocated, %bci_0 ], !is_relocation_phi !0
; CHECK-NOT: %relocated
  br label %not_zero625.us
}

attributes #0 = { noinline  }
attributes #2 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true"  }
