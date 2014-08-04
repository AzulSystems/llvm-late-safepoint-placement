; RUN: opt %s -S -place-safepoints -spp-use-vm-state=false | FileCheck %s

define fastcc void @test(i8 addrspace(1)* %arg) #0 {
entry:
; CHECK-LABEL: entry
  unreachable

; CHECK-NOT: other
; CHECK-NOT: statepoint
other:
  %tmp = bitcast i8 addrspace(1)* %arg to i32 addrspace(1)*
  call void undef()
  ret void
}

attributes #0 = { "gc-add-call-safepoints"="true" }
