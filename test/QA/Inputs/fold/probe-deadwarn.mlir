sea.program @main {
  %x = sea.nondet "x" [0] : i1
  %t = sea.const true
  %f = sea.xor %t, %t : i1
  sea.cond_br %f, ^dead, ^live
^dead:
  sea.br ^dead2
^dead2:
  sea.assume %f loc("dead.c":7:3)
  sea.br ^live
^live:
  sea.assert %x
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
