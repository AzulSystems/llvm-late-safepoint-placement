;; RUN: opt -merge-non-dominating-vmstates %s -S | FileCheck %s

;; This test that -merge-non-dominating-vmstates only affect the current
;; function it is working on and does not pollute other functions those
;; has uses of the global @llvm.jvmstate_anchor

%jObject = type { [8 x i8] }
%jMethod = type { [168 x i8] }

@llvm.jvmstate_anchor = private global i32 0

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, i32, i32, i8*) #1

declare void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]*)

; Function Attrs: uwtable
define i32 @"inline::test1"(i32, %jMethod addrspace(1)* nocapture readnone %method) #2 {
bci_0:
  %1 = tail call i32 @llvm.jvmstate_0(i32 %0, i32 0, i32 0, i32 1, i32 0, i32 99, i8* null)
  store volatile i32 %1, i32* @llvm.jvmstate_anchor, align 4
  %2 = load [1768 x i8]* addrspace(256)* inttoptr (i64 208 to [1768 x i8]* addrspace(256)*), align 16
  call void @"VMRuntime::poll_at_safepoint_static"([1768 x i8]* %2)
  ret i32 0
}

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_8(i32, i32, i32, i32, i32) #1

; Function Attrs: uwtable
define i32 @"inline::test2"(i32, %jMethod addrspace(1)* %method) #1 {
bci_0:
  %1 = load [1768 x i8]* addrspace(256)* inttoptr (i64 208 to [1768 x i8]* addrspace(256)*)
  %2 = call i32 @llvm.jvmstate_8(i32 %0, i32 0, i32 0, i32 0, i32 0)
  store volatile i32 %2, i32* @llvm.jvmstate_anchor
;; CHECK: store volatile i32 %2, i32* @llvm.jvmstate_anchor
  ret i32 20
}

attributes #1 = { nounwind readonly "gc-leaf-function"="true" }
attributes #2 = { uwtable "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
