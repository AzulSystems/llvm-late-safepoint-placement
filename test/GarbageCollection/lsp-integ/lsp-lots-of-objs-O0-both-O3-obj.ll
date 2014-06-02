; RUN: %p/../Inputs/clang-proxy.sh %p/../Inputs/lsp-lots-of-objs.cpp -S -emit-llvm -o %t -O0
; RUN: llvm-link %t %p/../Inputs/lsp-library.ll -S -o %t
; RUN: opt %t -S -o %t -place-call-safepoints -place-backedge-safepoints -spp-all-functions -O3
; RUN: llc -filetype=obj < %t
; RUN: llc < %t | FileCheck %s

; CHECK: StackMaps