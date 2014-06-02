; RUN: %p/../Inputs/clang-proxy.sh %p/../Inputs/lsp-placement.cpp -S -emit-llvm -o %t -O3
; RUN: llvm-link %t %p/../Inputs/lsp-library.ll -S -o %t
; RUN: opt %t -S -o %t -place-call-safepoints -spp-all-functions 
; RUN: llc -filetype=asm < %t
; RUN: llc < %t | FileCheck %s

; CHECK: StackMaps