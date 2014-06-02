; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-call-safepoints -spp-all-functions -spp-print-base-pointers -S 

declare void @call2safepoint()


define i64* @test(i64* %obj, i64* %obj2, i1 %condition) {
entry:
  call void @call2safepoint()
  br label %joint

joint:
  %phi1 = phi i64* [%obj, %entry], [%obj3, %joint2]
  br i1 %condition, label %use, label %joint2

use:
  br label %joint2

joint2:
  %phi2 = phi i64* [%obj, %use], [%obj2, %joint]
  %obj3 = getelementptr i64* %obj2, i32 1
  br label %joint
}