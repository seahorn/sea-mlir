// Input of sea-run.test: a guard that the dictionary passes, then a
// failing assert.
sea.program @main {
  %x = sea.nondet "nondet_u32" [0] : i32
  %c = sea.const 1234 : i32
  %eq = sea.icmp eq, %x, %c : i32
  sea.assume %eq
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
