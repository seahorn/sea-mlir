// RUN: sea-vc %s | FileCheck %s
// RUN: sea-vc %s --seam | FileCheck %s

// A counterexample replays with and without --seam: the interpreter's value
// of a segment is checked against the model's (obj.<name> = base).
// CHECK: result: sat
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
!am = !sea.mem<!sea.addr<64>, i8>
sea.program @x {
  %b = sea.segment "stack.3" base 3221225456 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [0] : !am
  %one = sea.const 1 : i64
  %q = sea.addr.add %b, %one : !sea.addr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v inplace : !am, i8
  %x = sea.load %m1[%b] : !am -> i8
  %c = sea.icmp eq, %x, %v : i8
  sea.assert %c
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
