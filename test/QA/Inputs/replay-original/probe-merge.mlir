sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %c = sea.nondet "c" [1] : i1
  %k = sea.const 5 : i8
  %ok = sea.icmp ne, %x, %k : i8
  sea.cond_br %c, ^l, ^r
^l:
  sea.assert %ok
  sea.halt
^r:
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
