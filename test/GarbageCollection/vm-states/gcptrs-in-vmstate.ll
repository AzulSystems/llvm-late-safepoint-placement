; RUN: opt -spp-no-entry -spp-no-backedge -place-safepoints -remove-fake-vmstate-calls -S %s | llc | FileCheck %s

; ModuleID = '<stdin>'

@llvm.jvmstate_anchor = private global i32 0

declare void @place_for_statepoint(i64 addrspace(1)* %oop, i64 %arg1, i64 %arg2, i64 %arg3, i64 %arg4, i64 %arg5, i64 %arg6, i64 %arg7, i64 %arg8, i64 %arg9)
declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, i32, i32, i64 addrspace(1)*)

define i64 @test(i64 addrspace(1)* %oop, i64 %arg1, i64 %arg2, i64 %arg3, i64 %arg4, i64 %arg5, i64 %arg6, i64 %arg7, i64 %arg8, i64 %arg9) #0 { 
entry:
  %0 = add i64 %arg1, %arg2
  %1 = add i64 %arg2, %arg3
  %2 = add i64 %arg3, %arg4
  %3 = add i64 %arg4, %arg5
  %4 = add i64 %arg5, %arg6
  %5 = add i64 %arg6, %arg7
  %6 = add i64 %arg7, %arg8
  %7 = add i64 %arg8, %arg9

  %tmp = load i64 addrspace(1)* %oop
  %8 = add i64 %tmp, %arg1

  %a = call i32 @llvm.jvmstate_0(i32 0, i32 0, i32 0, i32 1, i32 0, i32 12, i64 addrspace(1)* %oop)
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  
  call void @place_for_statepoint(i64 addrspace(1)* %oop, i64 %0, i64 %1, i64 %2, i64 %3, i64 %4, i64 %5, i64 %6, i64 %7, i64 %8)
; CHECK: #STATEPOINT
; CHECK: Indirect RSP + 
; CHECK: Indirect RSP + 
; CHECK: Indirect RSP + 

  %n2 = add i64 %0, %1
  %n3 = add i64 %n2, %2
  %n4 = add i64 %n3, %3
  %n5 = add i64 %n4, %4
  %n6 = add i64 %n5, %5
  %n7 = add i64 %n6, %6
  %n8 = add i64 %n7, %7

  %n9 = add i64 %n8, %8

  %tmp1 = getelementptr i64 addrspace(1)* %oop, i32 1
  %tmp2 = load i64 addrspace(1)* %tmp1
  %res = add i64 %n9, %tmp2

  ret i64 %res
}

attributes #0 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
