sea.program @p {
  %x = sea.nondet "x" [0] : i8 loc("m.c":3:3)
  %z = sea.const 0 : i8
  %c = sea.icmp eq, %x, %z : i8 loc("m.c":4:7)
  sea.cond_br %c, ^a, ^b loc("m.c":4:3)
^a:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
^b:
  %g = sea.const 0 : i1
  sea.assume %g
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
