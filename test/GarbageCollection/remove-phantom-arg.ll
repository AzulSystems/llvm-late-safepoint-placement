; RUN: opt %s -remove-phantom-argument -S 2>&1 | FileCheck %s
; This tests that we can remove the first phantom argument
; which is i32 as a vmstate. 

; ModuleID = '<stdin>'

declare void @"some_call"(i64*)

define void @test(i32 %phantom, i64* %obj, i64 %tmp) #0 {
; CHECK: define void @test(i64* %obj, i64 %tmp) 
entry:
  br label %loop

loop:
  call void @"some_call"(i64* %obj) 
  %cmp = icmp eq i64 %tmp, 0
  br i1 %cmp, label %loop, label %return    

return: 
  ret void
}

attributes #0 = { nounwind "gc-add-call-safepoints"="true" }

