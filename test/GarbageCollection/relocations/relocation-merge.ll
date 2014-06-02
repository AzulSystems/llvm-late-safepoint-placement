; RUN: opt %s -place-call-safepoints -spp-all-functions -S 2>&1 | FileCheck %s

%jObject = type { [8 x i8] }

declare  void @"some_call"(%jObject addrspace(1)*)

; Function Attrs: nounwind
define  void @"relocate_merge"(%jObject addrspace(1)* %arg) #1 {
bci_0:
  br i1 undef, label %if_branch, label %else_branch

if_branch:
  call void @"some_call"(%jObject addrspace(1)* %arg)
; CHECK: statepoint
  br label %join

else_branch:
  call void @"some_call"(%jObject addrspace(1)* %arg)
; CHECK: statepoint
  br label %join

join:
; We need to end up with a single relocation phi updated from both paths 
; here.  If both paths independently insert relocation phis, the second
; one will not update the input of the first one correctly (since that
; path wasn't reachable from the second safepoint)
; CHECK: join
; CHECK: phi
; CHECK: is_relocation_phi
; CHECK-NOT: phi
  call void @"some_call"(%jObject addrspace(1)* %arg)
; CHECK: statepoint
  ret void
}
