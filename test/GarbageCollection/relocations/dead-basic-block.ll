; RUN: llvm-link %s %p/../Inputs/lsp-library.ll -S | opt -spp-all-functions -place-call-safepoints -verify-safepoint-ir -S

;; Source: com.sun.crypto.provider.DESCrypt::cipherBlock

define void @test(i64* %obj) {
entry:
  call void undef(i64 undef)
  call i32 undef(i64* %obj)
  br label %common_block

common_block:
  ret void

dead_block:
  br label %common_block
}
