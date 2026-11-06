// load-to-nondet: a load becomes a nondet of its type with a fresh site
// (one past the largest of the program: sites stay unique, which the
// verifier of the second sea-opt checks); a pointer load becomes
// sea.ptr.nondet.
// RUN: sea-opt %s --sea-reduce-apply="pattern=load-to-nondet" | sea-opt | FileCheck %s

// CHECK-LABEL: sea.program @main
// CHECK-NOT: sea.load
// CHECK:     sea.ptr.nondet "sea.reduce" [6] : !sea.ptr<64>
// CHECK:     sea.nondet "sea.reduce" [7] : i32
// CHECK-NOT: sea.load

sea.program @main {
  %m0 = sea.mem.nondet "heap" [0] : !sea.mem<!sea.ptr<64>, i8>
  %g = sea.global @g size 16 align 8 : !sea.ptr<64>
  %q = sea.load %m0[%g] : !sea.mem<!sea.ptr<64>, i8> -> !sea.ptr<64>
  %x = sea.load %m0[%q] : !sea.mem<!sea.ptr<64>, i8> -> i32
  %h = sea.nondet "nd" [5] : i32
  %z = sea.icmp ne, %x, %h : i32
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
