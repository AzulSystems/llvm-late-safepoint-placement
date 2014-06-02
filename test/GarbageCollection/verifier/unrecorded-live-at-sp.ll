; RUN: opt %s -verify-safepoint-ir -S || echo "Crash" 2>&1 | FileCheck %s

; CHECK: Crash

%jNotAtSP = type { [8 x i8] }
%jObject = type { [8 x i8] }

@llvm.jvmstate_anchor = private global i32 0

declare %jObject addrspace(1)* @generate_obj1() #1

declare %jObject addrspace(1)* addrspace(1)* @generate_obj2() #1

declare %jNotAtSP addrspace(1)* @generate_obj3() #1

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, i32, i8*, i8*, i8*, i8*, i8*, i8*, i8*) #2

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_52(i32, i32, i32, i32, i32, i32, i32, i32, %jObject addrspace(1)*, i32) #2

; Function Attrs: nounwind
define  void @"java.util.HashMap::transfer"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)*, %jObject addrspace(1)*, i32) #3 {
bci_0:
  %3 = tail call i32 @llvm.jvmstate_0(i32 0, i32 0, i32 10, i32 0, %jObject addrspace(1)* %0, %jObject addrspace(1)* %1, i32 %2, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null)
  %result608 = call %jNotAtSP addrspace(1)* @generate_obj3()
  %obj609 = bitcast %jNotAtSP addrspace(1)* %result608 to %jObject addrspace(1)*
  %cast = bitcast %jNotAtSP addrspace(1)* %result608 to %jObject addrspace(1)*
  %cast5 = bitcast %jNotAtSP addrspace(1)* %result608 to %jObject addrspace(1)*
  br label %bci_37-aload

bci_37-aload:                                     ; preds = %not_zero179, %bci_0
  %base_phi = phi %jObject addrspace(1)* [ %base_phi1.relocated, %not_zero179 ], [ %cast, %bci_0 ], !is_base_value !0
  %base_phi2 = phi %jObject addrspace(1)* [ %base_phi3, %not_zero179 ], [ %cast5, %bci_0 ], !is_base_value !0
  %relocated8 = phi %jObject addrspace(1)* [ %relocated7.relocated, %not_zero179 ], [ %obj609, %bci_0 ]
  %4 = getelementptr inbounds %jObject addrspace(1)* %relocated8, i64 0, i32 0, i64 32
  %addr98 = bitcast i8 addrspace(1)* %4 to %jObject addrspace(1)* addrspace(1)*
  %cast6 = bitcast %jObject addrspace(1)* %base_phi2 to %jObject addrspace(1)* addrspace(1)*
  br i1 undef, label %not_zero179, label %not_zero146

not_zero146:                                      ; preds = %bci_37-aload
  %addr98.relocated = call %jObject addrspace(1)* addrspace(1)* @generate_obj2() #1
  %obj609.relocated = call %jObject addrspace(1)* @generate_obj1() #1
  br label %not_zero179

not_zero179:                                      ; preds = %not_zero146, %bci_37-aload
  %base_phi1 = phi %jObject addrspace(1)* [ %obj609.relocated, %not_zero146 ], [ %base_phi, %bci_37-aload ], !is_base_value !0
  %base_phi3 = phi %jObject addrspace(1)* [ %obj609.relocated, %not_zero146 ], [ %base_phi2, %bci_37-aload ], !is_base_value !0
  %relocated7 = phi %jObject addrspace(1)* [ %obj609.relocated, %not_zero146 ], [ %relocated8, %bci_37-aload ]
  %base_phi4 = phi %jObject addrspace(1)* addrspace(1)* [ %addr98.relocated, %not_zero146 ], [ %cast6, %bci_37-aload ], !is_base_value !0
  %relocated4 = phi %jObject addrspace(1)* addrspace(1)* [ %addr98.relocated, %not_zero146 ], [ %addr98, %bci_37-aload ]
  %safepoint_token = tail call  i32 (i32 ()*, i32, i32, i32, i32, i32, i32, ...)* @llvm.statepoint.p0f_i32f(i32 ()* undef, i32 0, i32 245, i32 0, i32 0, i32 10, i32 0, %jObject addrspace(1)* %0, %jObject addrspace(1)* %1, i32 %2, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null, %jObject addrspace(1)* %base_phi1, %jObject addrspace(1)* addrspace(1)* %base_phi4, %jObject addrspace(1)* addrspace(1)* %relocated4, %jObject addrspace(1)* %relocated7)
  %5 = call i32 @llvm.gc.result.int.i32(i32 %safepoint_token)
  %base_phi1.relocated = call coldcc %jObject addrspace(1)* @llvm.gc.relocate.p1jObject(i32 %safepoint_token, i32 17, i32 17)
  %base_phi4.relocated = call coldcc %jObject addrspace(1)* addrspace(1)* @llvm.gc.relocate.p1p1jObject(i32 %safepoint_token, i32 18, i32 18)
  %relocated4.relocated = call coldcc %jObject addrspace(1)* addrspace(1)* @llvm.gc.relocate.p1p1jObject(i32 %safepoint_token, i32 18, i32 19)
  %relocated7.relocated = call coldcc %jObject addrspace(1)* @llvm.gc.relocate.p1jObject(i32 %safepoint_token, i32 17, i32 20)
  %addr636 = bitcast %jObject addrspace(1)* addrspace(1)* %relocated4.relocated to %jNotAtSP addrspace(1)* addrspace(1)*
  %6 = tail call i32 @llvm.jvmstate_52(i32 101, i32 0, i32 10, i32 0, i32 %2, i32 undef, i32 undef, i32 0, %jObject addrspace(1)* %relocated7.relocated, i32 %5)
  br label %bci_37-aload
}

declare i32 @llvm.statepoint.p0f_i32f(i32 ()*, i32, i32, i32, i32, i32, i32, ...)

; Function Attrs: nounwind
declare i32 @llvm.gc.result.int.i32(i32) #4

; Function Attrs: nounwind
declare %jObject addrspace(1)* @llvm.gc.relocate.p1jObject(i32, i32, i32) #4

; Function Attrs: nounwind
declare %jObject addrspace(1)* addrspace(1)* @llvm.gc.relocate.p1p1jObject(i32, i32, i32) #4

attributes #0 = { noinline nounwind "gc-leaf-function"="true" }
attributes #1 = { "gc-leaf-function"="true" }
attributes #2 = { nounwind readonly "gc-leaf-function"="true" }
attributes #3 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true" }
attributes #4 = { nounwind }

!0 = metadata !{i32 1}
