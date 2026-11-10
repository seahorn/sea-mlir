
sea.program @no_merge {
  %c = sea.nondet "c" [0] : i1
  sea.cond_br %c, ^l, ^r
^l:
  sea.br ^j
^r:
  sea.br ^j
^j:
  sea.br ^e loc("f.c":30:3)
^e:
  %f = sea.const false
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
