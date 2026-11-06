// RUN: not sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static))' -split-input-file 2>&1 | FileCheck %s
// RUN: not sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static{reuse=true}))' 2>&1 | FileCheck %s --check-prefix=REUSE

// CHECK: Error: sea-place-static: an allocation inside a loop cannot be placed statically (unroll first)
// CHECK-SAME: [place.alloc-in-loop]
sea.program @loop {
  %n = sea.const 1 : i64
  sea.br ^head
^head:
  %p = sea.alloca %n align 1 [0] : i64 -> !sea.ptr<64>
  sea.br ^head
}

// -----

// CHECK: Error: sea-place-static: no room for stack.1 (200 units) in a 8-bit address space
// CHECK-SAME: [place.out-of-space]
sea.program @full {
  %n = sea.const 200 : i8
  %p = sea.alloca %n align 1 [0] : i8 -> !sea.ptr<8>
  %q = sea.alloca %n align 1 [1] : i8 -> !sea.ptr<8>
  sea.halt
}

// REUSE: Error: sea-place-static: reuse=true is not implemented
// REUSE-SAME: [place.reuse-unimplemented]

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
