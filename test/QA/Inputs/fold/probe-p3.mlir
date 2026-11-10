sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %t = sea.const true
  sea.assert %t loc("real.c":8:5)
  %c = sea.icmp ne, %x, %x : i8 loc("real.c":9:5)
  sea.assert %c loc("real.c":9:5)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
