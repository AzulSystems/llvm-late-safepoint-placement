; RUN: llc %s -safepoint-machineInstr-verifier-print-only -verify-safepoint-machineinstrs  2>&1 | FileCheck %s

; CHECK:      Illegal use of unrelocated machine value after safepoint found!
; CHECK-NEXT: MachineInstr:   MOV64mr <fi#0>, 1, %noreg, 0, %noreg, %vreg0; mem:ST8[FixedStack0] GR64:%vreg0
; CHECK-NEXT: Illegal use of unrelocated machine value after safepoint found!
; CHECK-NEXT: MachineInstr:   %vreg2<def> = MOV64rm %vreg0, 1, %noreg, 8, %noreg; mem:LD8[%sunkaddr2(addrspace=1)] GR64:%vreg2,%vreg0
; CHECK-NEXT: Illegal use of unrelocated machine value after safepoint found!
; CHECK-NEXT: MachineInstr:   MOV64mr <fi#0>, 1, %noreg, 0, %noreg, %vreg0; mem:ST8[FixedStack0] GR64:%vreg0
; CHECK-NEXT: Illegal use of unrelocated machine value after safepoint found!
; CHECK-NEXT: MachineInstr:   %vreg2<def> = MOV64rm %vreg0, 1, %noreg, 8, %noreg; mem:LD8[%sunkaddr2(addrspace=1)] GR64:%vreg2,%vreg0


; ModuleID = '<stdin>'

define i64 @test(i64 addrspace(1)* %obj, i64 %tmp) {
entry:
  br label %loop

loop:                                             ; preds = %safepointblock, %entry
  %relocated = phi i64 addrspace(1)* [ %relocated, %safepointblock ], [ %obj, %entry ], !is_relocation_phi !0, !is_base_value !0
  %notneed = getelementptr i64 addrspace(1)* %relocated, i32 1
  %cmp = icmp eq i64 %tmp, 0
  br i1 %cmp, label %useblock, label %safepointblock

useblock:                                         ; preds = %loop
  %notneed.lcssa = phi i64 addrspace(1)* [ %notneed, %loop ]
  %result = load i64 addrspace(1)* %notneed.lcssa
  ret i64 %result

safepointblock:                                   ; preds = %loop
  %safepoint_token = call i32 (void ()*, i32, i32, i32, i32, i32, i32, i32, ...)* @llvm.statepoint.p0f_isVoidf(void ()* @do_safepoint, i32 0, i32 0, i32 0, i32 -1, i32 0, i32 0, i32 0, i64 addrspace(1)* %relocated)
  %obj.relocated = call coldcc i64 addrspace(1)* @llvm.gc.relocate.p1i64(i32 %safepoint_token, i32 8, i32 8)
  br label %loop
}

declare void @do_safepoint()

define void @gc.safepoint_poll() {
entry:
  call void @do_safepoint()
  ret void
}

declare i32 @llvm.statepoint.p0f_isVoidf(void ()*, i32, i32, i32, i32, i32, i32, i32, ...)

; Function Attrs: nounwind
declare i64 addrspace(1)* @llvm.gc.relocate.p1i64(i32, i32, i32) #0

attributes #0 = { nounwind }

!0 = metadata !{i32 1}
