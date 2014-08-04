;; RUN: opt -merge-non-dominating-vmstates %s -S | FileCheck %s

declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, i32)

@llvm.jvmstate_anchor = private global i32 0

declare void @parse_point()

define void @merge_vmstate() #0 {
; CHECK: entry:
; CHECK: %0 = call i32 @llvm.jvmstate_0(i32 0, i32 0, i32 0, i32 0, i32 0)
entry:
  call i32 @llvm.jvmstate_0(i32 0, i32 0, i32 0, i32 0, i32 0)
  store volatile i32 %0, i32* @llvm.jvmstate_anchor
  br i1 undef, label %side, label %merge

; CHECK: side:
; CHECK: store volatile i32 %0, i32* @llvm.jvmstate_anchor
side:
  call void @parse_point()
  br label %merge

merge:
; CHECK: merge:
; CHECK: store volatile i32 %0, i32* @llvm.jvmstate_anchor
  call void @parse_point()
  ret void
}

attributes #0 = { nounwind "gc-add-call-safepoints"="true" }