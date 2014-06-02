; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-call-safepoints -spp-all-functions -S  | FileCheck %s

declare void @call2safepoint()


define i64* @test(i64* %obj, i64* %obj2, i1 %condition) {
entry:
  br i1 %condition, label %branch2, label %join

branch2:
  br i1 %condition, label %callbb, label %join2

callbb:
  call void @call2safepoint()
  br label %join

join:
; CHECK: join
  ; This is a phi outside the dominator region of the new defs inserted by
  ; the safepoint, BUT we can't stop the search here or we miss the second
  ; phi below.
  %phi1 = phi i64* [%obj, %entry], [%obj2, %callbb]
; CHECK: %phi1 = phi i64* [ %obj, %entry ], [ %obj2.relocated, %callbb ]
  br label %join2

join2:
; CHECK: join2
  %phi2 = phi i64* [%obj, %join], [%obj2, %branch2]
; CHECK: %phi2 = phi i64* [ %relocated, %join ], [ %obj2, %branch2 ]
  ret i64* %phi2
}
