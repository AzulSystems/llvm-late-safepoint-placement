; RUN: opt %s -remove-redundant-vm-states -S

; ModuleID = 'zilla-1771.ll'

%jObject = type { [8 x i8] }

@llvm.jvmstate_anchor = private global i32 0

declare i32 @llvm.jvmstate_15()

declare i32 @llvm.jvmstate_120()

declare i32 @llvm.jvmstate_134()

define void @nested_loop() {
entry:
  %0 = call i32 @llvm.jvmstate_15()
  store volatile i32 %0, i32* @llvm.jvmstate_anchor 
  %1 = call i32 @llvm.jvmstate_15()
  store volatile i32 %1, i32* @llvm.jvmstate_anchor
  br label %outer.loop

outer.loop:                                       ; preds = %post.inner.loop, %entry
  br label %branch

branch:                                           ; preds = %inner.loop.loopback, %outer.loop
  br i1 undef, label %inner.loop, label %skip.inner.loop

skip.inner.loop:                                  ; preds = %branch
  br label %post.inner.loop

inner.loop:                                       ; preds = %branch
  %2 = call i32 @llvm.jvmstate_120()
  store volatile i32 %2, i32* @llvm.jvmstate_anchor
  br label %inner.loop.loopback

inner.loop.loopback:                              ; preds = %inner.loop
  br i1 undef, label %inner.loop.exit, label %branch

inner.loop.exit:                                  ; preds = %inner.loop.loopback
  store %jObject addrspace(1)* undef, %jObject addrspace(1)* addrspace(1)* undef
  br label %post.inner.loop

post.inner.loop:                                  ; preds = %inner.loop.exit, %skip.inner.loop
  %3 = call i32 @llvm.jvmstate_134()
  store volatile i32 %3, i32* @llvm.jvmstate_anchor
  br label %outer.loop
}