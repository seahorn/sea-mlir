sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i1
  sea.assert %y loc("real.c":8:5)
  %one = sea.const 1 : i8
  %c = sea.icmp ne, %one, %one : i8 loc("real.c":9:5)
  sea.assert %c loc("real.c":9:5)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
