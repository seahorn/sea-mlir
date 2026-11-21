// RUN: not sea-opt %s --sea-check-inb 2>&1 | FileCheck %s

// A box that reaches sea-check-inb was not lowered first: its loads would
// go unchecked. An error, not a silent skip.
// CHECK: Error: {{.*}}sea.box.all is left{{.*}}[check-inb.unsupported]
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unlowered {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i8
  %all = sea.box.all (%i) = (0) to (%c16) {
    %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %x = sea.load %m0[%p] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
