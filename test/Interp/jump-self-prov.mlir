// RUN: sea-run %s | FileCheck %s

// An address argument fed by itself on a back edge keeps its provenance.
sea.program @provself {
  %A = sea.segment "heap.1" base 16 : i32 size 8 : i32 align 8 kind "heap" : !sea.addr<32>
  %t = sea.const 1 : i1
  sea.br ^h(%A, %t : !sea.addr<32>, i1)
^h(%x : !sea.addr<32> loc("x"), %first : i1):
  %f = sea.const 0 : i1
  sea.cond_br %first, ^h(%x, %f : !sea.addr<32>, i1), ^use
^use:
  // CHECK: print: x = &heap.1[0]
  sea.print "x" = "x" : !sea.addr<32>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
