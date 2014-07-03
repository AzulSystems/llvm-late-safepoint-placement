; RUN: opt %s -S -O3 | FileCheck %s

; CHECK-NOT: calloc
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Function Attrs: uwtable
define void @_Z3foov() #0 {
entry:
  %call = tail call i8* @_Z9my_calloci(i32 5)
  ret void
}

declare i8* @_Z9my_calloci(i32) #1

attributes #0 = { uwtable }
attributes #1 = { "calloc_like" }
