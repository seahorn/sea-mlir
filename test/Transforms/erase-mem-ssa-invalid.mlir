// RUN: not sea-opt %s --sea-erase-mem-ssa -split-input-file 2>&1 | FileCheck %s

// sea-erase-mem-ssa refuses what one linear region cannot express.

// A load of an earlier version.
// CHECK: erase-mem-ssa.non-linear
sea.program @old_version {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%p], %v : !sea.mem<!sea.ptr<64>, i8>, i8
  %x = sea.load %m0[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.halt
}

// -----

// A version consumed twice on one path.
// CHECK: erase-mem-ssa.non-linear
sea.program @twice {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%p], %v : !sea.mem<!sea.ptr<64>, i8>, i8
  %m2 = sea.store %m0[%p], %v : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.halt
}

// -----

// A join without a memory argument whose predecessors leave different
// versions, one of which is read after it.
// CHECK: erase-mem-ssa.non-linear
sea.program @join_no_arg {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %c = sea.nondet "c" [2] : i1
  sea.cond_br %c, ^a, ^j
^a:
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%p], %v : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.br ^j
^j:
  %x = sea.load %m0[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.halt
}

// -----

// Two independent memories.
// CHECK: 2 memories{{.*}}[erase-mem-ssa.regions]
sea.program @two {
  %a = sea.mem.nondet "a" [0] : !sea.mem<!sea.ptr<64>, i8>
  %b = sea.mem.nondet "b" [1] : !sea.mem<!sea.ptr<64>, i8>
  sea.halt
}

// -----

// A memmove between two memories is two regions too.
// CHECK: erase-mem-ssa.regions
sea.program @two_move {
  %a = sea.mem.nondet "a" [0] : !sea.mem<!sea.ptr<64>, i8>
  %b = sea.mem.nondet "b" [1] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  %n = sea.const 4 : i64
  %a1 = sea.memmove %a[%p], %b[%p], %n inplace : !sea.mem<!sea.ptr<64>, i8>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
