;; RUN: opt -remove-redundant-vm-states < %s
;; Regression test for Zilla 1771

%jObject = type { [8 x i8] }

declare void @llvm.jvmstate_15()

declare void @llvm.jvmstate_120()

declare void @llvm.jvmstate_134()

;; Reduced from java.security.AccessControlContext::optimize

define void @nested_loop() {
entry:
  call void @llvm.jvmstate_15()
  br label %outer.loop

outer.loop:
  br label %branch

branch:
  br i1 undef, label %inner.loop, label %skip.inner.loop

skip.inner.loop:
  br label %post.inner.loop

inner.loop:
  call void @llvm.jvmstate_120()
  br label %inner.loop.loopback

inner.loop.loopback:
  br i1 undef, label %inner.loop.exit, label %branch

inner.loop.exit:
  store %jObject addrspace(1)* undef, %jObject addrspace(1)* addrspace(1)* undef
  br label %post.inner.loop

post.inner.loop:
  call void @llvm.jvmstate_134()
  br label %outer.loop
}
