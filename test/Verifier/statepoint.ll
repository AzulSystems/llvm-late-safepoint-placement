; RUN: opt -S %s -O3 | FileCheck %s
; This test catches two cases where the verifier was too strict:
; 1) A base doesn't need to be relocated if it's never used again
; 2) A value can be replaced by one which is known equal.  This
; means a potentially derived pointer can be known base and that
; we can't check that derived pointer are never bases.

declare void @use(...)
declare i64 addrspace(1)* @llvm.gc.relocate.p1i64(i32, i32, i32)
declare i32 @llvm.statepoint.p0f_isVoidf(void ()*, i32, i32, i32, i32, i32, i32, i32, ...)

define void @example(i8 addrspace(1)* %arg, i64 addrspace(1)* %arg2) {
entry:
  %cast = bitcast i8 addrspace(1)* %arg to i64 addrspace(1)*
  %c = icmp eq i64 addrspace(1)* %cast,  %arg2
  br i1 %c, label %equal, label %notequal

notequal:
  ret void

equal:
  %safepoint_token = call i32 (void ()*, i32, i32, i32, i32, i32, i32, i32, ...)* @llvm.statepoint.p0f_isVoidf(void ()* undef, i32 0, i32 0, i32 0, i32 0, i32 0, i32 10, i32 0, i8 addrspace(1)* %arg, i64 addrspace(1)* %cast, i8 addrspace(1)* %arg, i8 addrspace(1)* %arg)
; CHECK-LABEL: equal
; CHECK: statepoint
; CHECK-NOT: cast
  %reloc = call coldcc i64 addrspace(1)* @llvm.gc.relocate.p1i64(i32 %safepoint_token, i32 8, i32 9)
  call coldcc void undef(i64 addrspace(1)* %reloc)
  ret void
}
