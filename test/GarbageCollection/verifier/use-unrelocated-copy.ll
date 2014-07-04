; RUN: llc %s -verify-safepoint-machineinstrs || echo "Crash" 2>&1 | FileCheck %s

; CHECK: Crash

; ModuleID = '<stdin>'

declare  void @"some_call"(i64*)

define i64 @test(i64* %obj, i64 %tmp, i64* %objj) {
entry:
  br label %loop

loop:                                             ; preds = %safepointblock, %entry
  %relocated = phi i64* [ %obj.relocated, %safepointblock ], [ %obj, %entry ], !is_relocation_phi !0, !is_base_value !0
  %relocated_copy = bitcast i64* %relocated to i64*
  %notneed = getelementptr i64* %relocated, i32 1
  %cmp = icmp eq i64 %tmp, 0
  br i1 %cmp, label %useblock, label %safepointblock

useblock:                                         ; preds = %loop
  %notneed.lcssa = phi i64* [ %notneed, %loop ]
  %result = load i64* %notneed.lcssa
  ret i64 %result

safepointblock:                                   ; preds = %loop
  %safepoint_token = call i32 (void ()*, i32, i32, i32, i32, i32, i32, ...)* @llvm.statepoint.p0f_isVoidf(void ()* @do_safepoint, i32 0, i32 0, i32 -1, i32 0, i32 0, i32 0, i64* %relocated)
  %obj.relocated = call coldcc i64* @llvm.gc.relocate.p0i64(i32 %safepoint_token, i32 7, i32 7)
  tail call void @"some_call"(i64* %relocated_copy) 
  br label %loop
}

declare void @do_safepoint()

define void @gc.safepoint_poll() {
entry:
  call void @do_safepoint()
  ret void
}

declare i32 @llvm.statepoint.p0f_isVoidf(void ()*, i32, i32, i32, i32, i32, i32, ...)

; Function Attrs: nounwind
declare i64* @llvm.gc.relocate.p0i64(i32, i32, i32) #0

attributes #0 = { nounwind }

!0 = metadata !{i32 1}
