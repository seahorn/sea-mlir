sea.program @main {
  %c = sea.nondet "c" [0] : i1
  sea.cond_br %c, ^a, ^b
^a:
  sea.br ^b
^b:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
