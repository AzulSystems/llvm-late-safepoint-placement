; RUN: not opt -S %s 2>&1 | FileCheck %s
; CHECK: relocating non gc

declare void @use(...)
declare i64 addrspace(1)* @llvm.gc.relocate.p1i64(i32, i32, i32)
declare i32 @llvm.statepoint.p0f_isVoidf(void ()*, i32, i32, i32, i32, i32, i32, ...)

define void @example(i8 addrspace(1)* %arg) {
entry:
  %cast = bitcast i8 addrspace(1)* %arg to i64 addrspace(1)*
  %safepoint_token = call i32 (void ()*, i32, i32, i32, i32, i32, i32, ...)* @llvm.statepoint.p0f_isVoidf(void ()* undef, i32 0, i32 0, i32 0, i32 0, i32 10, i32 0, i8 addrspace(1)* %arg, i64 addrspace(1)* %cast)
; The indices are off by one in this relocate
  %reloc = call coldcc i64 addrspace(1)* @llvm.gc.relocate.p1i64(i32 %safepoint_token, i32 5, i32 6)
  ret void
}
