;; RUN: opt -place-safepoints -remove-fake-vmstate-calls %s -S | FileCheck %s

declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, i32, i32, i8*)

@llvm.jvmstate_anchor = private global i32 0

declare void @parse_point()

define void @merge_vmstate(i8* %w, i8* %x, i8* %y, i8* %z) #0 {
entry:
  switch i32 undef, label %up [ i32 0, label %down
                                i32 1, label %left
                                i32 2, label %right ]

up:
  %a0 = call i32 @llvm.jvmstate_0(i32 0, i32 1, i32 1, i32 0, i32 0, i32 0, i8* %w)
  store volatile i32 %a0, i32* @llvm.jvmstate_anchor
  br label %merge_x

down:
  %b0 = call i32 @llvm.jvmstate_0(i32 0, i32 1, i32 1, i32 0, i32 0, i32 0, i8* %x)
  store volatile i32 %b0, i32* @llvm.jvmstate_anchor
  br label %merge_x

left:
  %c0 = call i32 @llvm.jvmstate_0(i32 0, i32 1, i32 1, i32 0, i32 0, i32 0, i8* %y)
  store volatile i32 %c0, i32* @llvm.jvmstate_anchor
  br label %merge_y

right:
  %d0 = call i32 @llvm.jvmstate_0(i32 0, i32 1, i32 1, i32 0, i32 0, i32 0, i8* %z)
  store volatile i32 %d0, i32* @llvm.jvmstate_anchor
  br label %merge_y

merge_x:
; CHECK: merge_x:
; CHECK:  [[PHI_X:%.*]] = phi i8* [ %w, %up ], [ %x, %down ]
  br label %merge_z

merge_y:
; merge_y:
; CHECK: [[PHI_Y:%.*]] = phi i8* [ %z, %right ], [ %y, %left ]
  br label %merge_z

merge_z:
; merge_z:

; CHECK: [[PHI_Z:%.*]] = phi i8* [ [[PHI_X]], %merge_x ], [ [[PHI_Y]], %merge_y ]
; CHECK: llvm.statepoint.p0f_isVoidf(void ()* @parse_point, i32 0, i32 0, i32 0, i32 1, i32 1, i32 0, i32 0, i32 0, i8* [[PHI_Z]])
  call void @parse_point()
  ret void
}

attributes #0 = { nounwind "gc-add-call-safepoints"="true" }
