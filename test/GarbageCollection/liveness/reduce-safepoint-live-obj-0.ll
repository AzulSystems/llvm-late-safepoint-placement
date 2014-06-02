; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -place-backedge-safepoints -spp-all-functions -S 

define i64 @test(i64* %obj, i64 %tmp) {
entry:
  br label %loop

loop:
  %notneed = getelementptr i64* %obj, i32 1
  %cmp = icmp eq i64 %tmp, 0
  br i1 %cmp, label %useblock, label %safepointblock

useblock:
  %result = load i64* %notneed
  ret i64 %result

safepointblock:
  br label %loop
}




