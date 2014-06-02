; RUN: opt %s  -place-backedge-safepoints -place-call-safepoints -spp-all-functions -verify -S 2>&1 | FileCheck %s

; This is simpilied from java.io.UnixFileSystem::canonicalize
; It is not a failing test case, but just added for better test when we try to improve the relocation implementation.

%jNotAtSP = type { [8 x i8] }
%jObject = type { [8 x i8] }
%jMethod = type { [160 x i8] }

; Function Attrs: noinline
define void @gc.safepoint_poll() #4 {
entry:
  br i1 undef, label %safepointed, label %do_safepoint

do_safepoint:                                     ; preds = %entry
  unreachable

safepointed:                                      ; preds = %entry
  ret void
}

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_8(i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, %jObject addrspace(1)*, i8*, i8*, i8*, i8*) #6

; Function Attrs: nounwind
define  %jObject addrspace(1)* @"java.io.UnixFileSystem::canonicalize"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)*, %jObject addrspace(1)*) #7 {
bci_0:
  %2 = tail call i32 @llvm.jvmstate_8(i32 16, i32 1, i32 6, i32 0, %jObject addrspace(1)* undef, %jObject addrspace(1)* %0, %jObject addrspace(1)* %1, i8* null, i8* null, i8* null, i8* null)
  br i1 undef, label %zero, label %not_zero, !prof !0

zero:                                             ; preds = %bci_0
  ret %jObject addrspace(1)* undef

not_zero:                                         ; preds = %bci_0
  br i1 undef, label %bci_134-aload_2.thread, label %bci_45-aload_0

bci_45-aload_0:                                   ; preds = %not_zero
  tail call void undef(i64 undef) #8
  br i1 undef, label %bci_134-aload_2.thread, label %not_zero82

not_zero82:                                       ; preds = %bci_45-aload_0
  br label %bci_138-aload_0

bci_134-aload_2.thread:                           ; preds = %bci_45-aload_0, %not_zero
  br label %bci_138-aload_0

bci_138-aload_0:                                  ; preds = %bci_134-aload_2.thread, %not_zero82
;CHECK: %relocated = phi %jObject addrspace(1)* [ %relocated1, %bci_134-aload_2.thread ], [ %3, %not_zero82 ], !is_relocation_phi !1, !is_base_value !1
  %3 = tail call i1 undef(%jObject addrspace(1)* %0) #8
  unreachable
}

attributes #4 = { noinline  }
attributes #6 = { nounwind readonly "gc-leaf-function"="true" }
attributes #7 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true"   }
attributes #8 = { nounwind }

!0 = metadata !{metadata !"branch_weights", i32 4, i32 64}
