sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %t = sea.const 1 : i1
  %g = sea.xor %t, %t : i1
  sea.assume %g loc("guard.c":3:3)
  %f = sea.const 0 : i1
  sea.assert %f loc("check.c":5:3)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
