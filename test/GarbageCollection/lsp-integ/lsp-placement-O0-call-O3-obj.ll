; RUN: %p/../Inputs/clang-proxy.sh %p/../Inputs/lsp-placement.cpp -S -emit-llvm -o %t -O0
; RUN: llvm-link %t %p/../Inputs/lsp-library.ll -S -o %t
; RUN: opt %t -S -o %t -spp-no-backedge -spp-no-entry -place-safepoints -spp-all-functions -O3
; RUN: llc -spp-all-functions -filetype=obj < %t
; RUN: llc -spp-all-functions < %t | FileCheck %s

; CHECK: StackMaps