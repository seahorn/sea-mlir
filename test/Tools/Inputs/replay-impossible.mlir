// A sat whose model the interpreter cannot run: the stores lack `inplace`
// (docs/design/Memory.md, In place), so sea-vc prints result: sat, then error
// vc.replay-impossible, and exits 1. Input of bench-replay.test.
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @"main" {
^bb0:
  %m0 = sea.mem.nondet "heap" [0] : !m
  %sz = sea.const 8 : i64
  %v = sea.malloc %sz align 8 [1] : i64 -> !sea.ptr<64>
  %one = sea.const 1 : i64
  %zero = sea.const 0 : i64
  %m1 = sea.store %m0[%v], %zero align 8 : !m, i64
  %m2 = sea.store %m1[%v], %one align 8 : !m, i64
  // reads the version before the second store
  %x = sea.load %m1[%v] align 8 : !m -> i64
  %c = sea.icmp eq, %x, %zero : i64
  %f = sea.xor %c, %c : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
