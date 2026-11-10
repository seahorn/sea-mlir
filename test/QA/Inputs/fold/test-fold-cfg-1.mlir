
sea.program @assume_false {
  %y = sea.nondet "before" [0] : i8
  %t = sea.const true
  %g = sea.xor %t, %t : i1
  sea.assume %g loc("f.c":10:3)
  %z = sea.nondet "after" [1] : i1
  sea.br ^err
^err:
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
