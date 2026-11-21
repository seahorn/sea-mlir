// RUN: sea-run %s | FileCheck %s

// A hand-placed program: A at 16, B at 32. Address block arguments swapped on
// a back edge keep their own provenance (a parallel copy): after one swap x
// is B and y is A.
!m = !sea.mem<!sea.addr<32>, i8>
sea.program @provswap {
  %A = sea.segment "heap.1" base 16 : i32 size 8 : i32 align 8 kind "heap" : !sea.addr<32>
  %B = sea.segment "heap.2" base 32 : i32 size 8 : i32 align 8 kind "heap" : !sea.addr<32>
  %t = sea.const 1 : i1
  sea.br ^h(%A, %B, %t : !sea.addr<32>, !sea.addr<32>, i1)
^h(%x : !sea.addr<32> loc("x"), %y : !sea.addr<32> loc("y"), %first : i1):
  %f = sea.const 0 : i1
  sea.cond_br %first, ^h(%y, %x, %f : !sea.addr<32>, !sea.addr<32>, i1), ^use
^use:
  // CHECK: print: x = &heap.2[0]
  sea.print "x" = "x" : !sea.addr<32>
  // CHECK: print: y = &heap.1[0]
  sea.print "y" = "y" : !sea.addr<32>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
