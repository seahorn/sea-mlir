sea.program @main {
  %a = sea.nondet "nd" [0] : i8
  %c1 = sea.const 1 : i8
  %d = sea.icmp ult, %a, %c1 : i8
  sea.cond_br %d, ^bb1, ^bb2
^bb1:
  sea.br ^bb2
^bb2:
  %z = sea.icmp ne, %a, %c1 : i8
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
