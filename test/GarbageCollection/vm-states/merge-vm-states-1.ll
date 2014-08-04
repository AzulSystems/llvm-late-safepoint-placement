;; RUN: opt -place-safepoints -remove-fake-vmstate-calls %s -S | FileCheck %s

declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, i32, i32, i8*)

@llvm.jvmstate_anchor = private global i32 0

declare void @parse_point()

define void @merge_vmstate(i8* %x, i8* %y) #0 {
entry:
  br i1 undef, label %left, label %right

left:
  %a0 = call i32 @llvm.jvmstate_0(i32 0, i32 1, i32 1, i32 0, i32 0, i32 0, i8* %x)
  store volatile i32 %a0, i32* @llvm.jvmstate_anchor
  br label %loop

right:
  %b0 = call i32 @llvm.jvmstate_0(i32 0, i32 1, i32 1, i32 0, i32 0, i32 0, i8* %y)
  store volatile i32 %b0, i32* @llvm.jvmstate_anchor
  br label %loop

loop:
; CHECK: [[PHI:%.*]] = phi i8* [ %x, %left ], [ %y, %right ]
; CHECK: @llvm.statepoint.p0f_isVoidf(void ()* @parse_point, i32 0, i32 0, i32 0, i32 1, i32 1, i32 0, i32 0, i32 0, i8* [[PHI]]
  call void @parse_point()
  br label %loop
}

attributes #0 = { nounwind "gc-add-call-safepoints"="true" }
