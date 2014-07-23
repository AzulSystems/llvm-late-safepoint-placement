;; RUN: opt -place-safepoints %s -S | FileCheck %s
;; XFAIL:

; This demonstrates an issue with tail duplication.  An optimization
; may transform:
;
;  jvmstate_a()
;  jvmstate_b()
;  call()
;
; to
;
;  jvmstate_a()
;  if (condition) { jvmstate_b(); } else { jvmstate_b(); }
;  call()
;
; In the first case we'd be picked jvmstate_b() as the vm state for
; call() but in the second case we'd pick the jvmstate_a(), because
; that is the dominating vm state in this case.

declare i32 @llvm.jvmstate_1(i32, i32, i32, i32, i32)
declare i32 @llvm.jvmstate_2(i32, i32, i32, i32, i32)

@llvm.jvmstate_anchor = private global i32 0

declare void @parse_point()

define void @nested_loop() #0 {
entry:
  %a = call i32 @llvm.jvmstate_1(i32 0, i32 0, i32 0, i32 0, i32 0)
  store volatile i32 %a, i32* @llvm.jvmstate_anchor
  br i1 undef, label %left, label %right

left:
  %b0 = call i32 @llvm.jvmstate_2(i32 0, i32 1, i32 0, i32 0, i32 0)
  store volatile i32 %b0, i32* @llvm.jvmstate_anchor
  br label %merge

right:
  %b1 = call i32 @llvm.jvmstate_2(i32 0, i32 1, i32 0, i32 0, i32 0)
  store volatile i32 %b1, i32* @llvm.jvmstate_anchor
  br label %merge

merge:
;; CHECK: @llvm.statepoint.p0f_isVoidf(void ()* @parse_point, i32 0, i32 0, i32 0, i32 1, i32 0, i32 0, i32 0)
  call void @parse_point()
  ret void
}

attributes #0 = { nounwind "gc-add-call-safepoints"="true" }
