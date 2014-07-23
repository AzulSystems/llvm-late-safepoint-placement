; RUN: opt %s -spp-no-entry -spp-no-backedge -place-safepoints -spp-all-functions -S -spp-reloc-via-alloca=0 2>&1 | FileCheck %s
; RUN: opt %s -spp-no-entry -spp-no-backedge -place-safepoints -spp-all-functions -S -spp-reloc-via-alloca=1 2>&1 | FileCheck --check-prefix CHECK-ALLOCA %s

%jObject = type { [8 x i8] }

declare  void @"some_call"(%jObject addrspace(1)*)

; Function Attrs: nounwind
define  void @"relocate_merge"(%jObject addrspace(1)* %arg) #1 {
bci_0:
  br i1 undef, label %if_branch, label %else_branch

if_branch:
; CHECK-LABEL: if_branch:
; CHECK: llvm.statepoint
  call void @"some_call"(%jObject addrspace(1)* %arg)
  br label %join

else_branch:
; CHECK-LABEL: else_branch:
; CHECK: statepoint
  call void @"some_call"(%jObject addrspace(1)* %arg)
  br label %join

join:
; We need to end up with a single relocation phi updated from both paths 
; here.  If both paths independently insert relocation phis, the second
; one will not update the input of the first one correctly (since that
; path wasn't reachable from the second safepoint)
; CHECK-LABEL: join:
; CHECK: phi %jObject addrspace(1)*
; CHECK-DAG: [ %arg.relocated, %if_branch ]
; CHECK-DAG: [ %arg.relocated2, %else_branch ]
; CHECK-DAG: !is_relocation_phi
; CHECK-NOT: phi
; CHECK-ALLOCA-LABEL: join:
; CHECK-ALLOCA: phi %jObject addrspace(1)*
; CHECK-ALLOCA-DAG: [ %arg.relocated, %if_branch ]
; CHECK-ALLOCA-DAG: [ %arg.relocated2, %else_branch ]
; CHECK-ALLOCA-NOT: phi

  call void @"some_call"(%jObject addrspace(1)* %arg)
; CHECK: statepoint
  ret void
}
