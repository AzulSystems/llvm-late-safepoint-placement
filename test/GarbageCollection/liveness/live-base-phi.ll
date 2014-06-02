; RUN:  opt %s -place-entry-safepoints -place-backedge-safepoints -place-call-safepoints -S

%jNotAtSP = type { [8 x i8] }
%jObject = type { [8 x i8] }
%jMethod = type { [160 x i8] }

@llvm.jvmstate_anchor = private global i32 0

; Function Attrs: noinline
define void @gc.safepoint_poll() #4 {
entry:
  br i1 undef, label %safepointed, label %do_safepoint

do_safepoint:                                     ; preds = %entry
  %stack_pointer = tail call i8* @llvm.stacksave()
  unreachable

safepointed:                                      ; preds = %entry
  ret void
}

; Function Attrs: nounwind
declare  i8* @llvm.stacksave() #5

declare  void @"Runtime::poll_at_safepoint_static"([1776 x i8]*)

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_0(i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, i32, i8*, i8*, i8*, i8*, i8*, i8*, i8*) #7

declare  void @"Runtime::verify_code_cache_pc"(i64)

declare %jNotAtSP addrspace(1)* @generate_obj3() #8

; Function Attrs: nounwind readonly
declare i32 @llvm.jvmstate_52(i32, i32, i32, i32, %jObject addrspace(1)*, %jObject addrspace(1)*, i32, i32, i32, i32, %jObject addrspace(1)*, i32) #7

; Function Attrs: nounwind
define  void @"java.util.HashMap::transfer"([160 x i8]* nocapture readnone %method, %jObject addrspace(1)*, %jObject addrspace(1)*, i32) #9 {
bci_0:
  %3 = tail call i32 @llvm.jvmstate_0(i32 0, i32 0, i32 10, i32 0, %jObject addrspace(1)* %0, %jObject addrspace(1)* %1, i32 %2, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null, i8* null)
  br label %not_zero

not_zero:                                         ; preds = %bci_0
  %length_gep.i = getelementptr inbounds %jObject addrspace(1)* %1, i64 0, i32 0, i64 8
  %result608 = call %jNotAtSP addrspace(1)* @generate_obj3()
  %obj609 = bitcast %jNotAtSP addrspace(1)* %result608 to %jObject addrspace(1)*
  br label %bci_37-aload

bci_37-aload:                                     ; preds = %not_zero179, %not_zero
  %local_7_83 = phi %jObject addrspace(1)* [ undef, %not_zero179 ], [ %obj609, %not_zero ]
  br label %not_zero107

not_zero107:                                      ; preds = %bci_37-aload
  %4 = getelementptr inbounds %jObject addrspace(1)* %local_7_83, i64 0, i32 0, i64 32
  %addr98 = bitcast i8 addrspace(1)* %4 to %jObject addrspace(1)* addrspace(1)*
  %result613 = call %jNotAtSP addrspace(1)* @generate_obj3()
  br i1 undef, label %not_zero179, label %not_zero146

not_zero146:                                      ; preds = %not_zero107
  %value621 = call %jNotAtSP addrspace(1)* @generate_obj3()
  tail call void @"Runtime::verify_code_cache_pc"(i64 undef) #5
  br label %not_zero179

not_zero179:                                      ; preds = %not_zero146, %not_zero107
  %stack_1_ = phi i32 [ undef, %not_zero146 ], [ 0, %not_zero107 ]
  tail call void @"Runtime::verify_code_cache_pc"(i64 undef) #5
  %5 = tail call  i32 undef() #5
  %length.i425 = load i32 addrspace(1)* undef, align 4
  %addr636 = bitcast %jObject addrspace(1)* addrspace(1)* %addr98 to %jNotAtSP addrspace(1)* addrspace(1)*
  %6 = tail call i32 @llvm.jvmstate_52(i32 101, i32 0, i32 10, i32 0, %jObject addrspace(1)* %0, %jObject addrspace(1)* %1, i32 %2, i32 undef, i32 undef, i32 0, %jObject addrspace(1)* %local_7_83, i32 %5)
  br label %bci_37-aload
}

attributes #0 = { alwaysinline nounwind readnone "gc-leaf-function"="true" }
attributes #1 = { noinline nounwind "gc-leaf-function"="true" }
attributes #2 = { alwaysinline nounwind readonly "gc-leaf-function"="true" }
attributes #3 = { noinline nounwind readnone "gc-leaf-function"="true" }
attributes #4 = { noinline }
attributes #5 = { nounwind }
attributes #6 = { nounwind readnone }
attributes #7 = { nounwind readonly "gc-leaf-function"="true" }
attributes #8 = { "gc-leaf-function"="true" }
attributes #9 = { nounwind "gc-add-backedge-safepoints"="true" "gc-add-call-safepoints"="true" "gc-add-entry-safepoints"="true"}

!0 = metadata !{metadata !"branch_weights", i32 4, i32 64}
!1 = metadata !{metadata !"branch_weights", i32 64, i32 4}
