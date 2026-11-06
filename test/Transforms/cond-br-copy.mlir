// Constant control flow removes an infeasible large copy before encoding memory.
// RUN: sea-opt %s --sccp --canonicalize | FileCheck %s
// CHECK-LABEL: sea.program @copy_branch
// CHECK-NOT: sea.cond_br
// CHECK-NOT: sea.memmove
// CHECK: sea.halt
// CHECK-NOT: sea.cond_br
// CHECK-NOT: sea.memmove

module {
  sea.program @copy_branch {
    %true = sea.const true
    %false = sea.const false
    %length = sea.const 128 : i64
    %memory = sea.mem.nondet "memory" [2] : !sea.mem<!sea.ptr<64>, i8>
    %scratch = sea.alloca %length align 16 [3] : i64 -> !sea.ptr<64>
    %source = sea.ptr.null : !sea.ptr<64>
    sea.cond_br %false, ^done, ^check
  ^check:
    sea.cond_br %true, ^join, ^copy
  ^copy:
    %copied = sea.memmove %memory[%scratch], %memory[%source], %length inplace align 16 : !sea.mem<!sea.ptr<64>, i8>
    sea.br ^join
  ^join:
    sea.br ^done
  ^done:
    sea.halt
  }
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
