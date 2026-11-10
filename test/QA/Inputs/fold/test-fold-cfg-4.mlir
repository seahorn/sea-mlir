
sea.program @assert_false {
  %f = sea.const false
  sea.assert %f loc("f.c":20:3)
  %z = sea.nondet "after" [0] : i1
  sea.assume %z
  sea.br ^next
^next:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
