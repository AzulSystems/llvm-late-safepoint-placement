;; RUN: opt -place-safepoints %s -S | FileCheck %s

; This test shows that we'll end up selecting the correct VM state
; even if the actual jvmstate calls get reordered.  We pick the vm
; state based on the dominating store to the anchor, not the dominatig
; jvmstate_ call, and this is semantically important.
;
; That said, this isn't an issue currently (i.e. we'll get the correct
; VM state even if we pick the dominating jvmstate_ call), because we
; mark jvmstate_'s as reading *all* memory, so they can't be reordered
; with respect to a volatile write, but if we later discover a way to
; mark them as reading only the memory passed to them, then this can
; become a real issue.

declare i32 @llvm.jvmstate_1(i32, i32, i32, i32, i32)
declare i32 @llvm.jvmstate_2(i32, i32, i32, i32, i32)

@llvm.jvmstate_anchor = private global i32 0

declare void @parse_point()

define void @nested_loop() #0 {
entry:
  %b = call i32 @llvm.jvmstate_2(i32 0, i32 0, i32 0, i32 0, i32 0)
  %a = call i32 @llvm.jvmstate_1(i32 0, i32 1, i32 0, i32 0, i32 0)

  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  store volatile i32 %b, i32* @llvm.jvmstate_anchor

;; CHECK: @llvm.statepoint.p0f_isVoidf(void ()* @parse_point, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0)
  call void @parse_point()

  ret void
}

attributes #0 = { nounwind "gc-add-call-safepoints"="true" }
