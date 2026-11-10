sea.program @main {
  sea.br ^bb1
^bb1:
  sea.br ^bb2
^bb2:
  %t = sea.const 1 : i1 loc("harness.c":47:5)
  %u = sea.const 1 : i1 loc("harness.c":47:5)
  %g = sea.xor %t, %u : i1 loc("harness.c":47:5)
  sea.assume %g loc("harness.c":47:5)
  sea.br ^bb3 loc("harness.c":47:5)
^bb3:
  sea.br ^bb4
^bb4:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
