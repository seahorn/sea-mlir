sea.program @main {
  %c = sea.nondet "c" [0] : i1
  %v = sea.nondet "v" [1] : i8
  %t = sea.const true
  sea.cond_br %c, ^a, ^b
^a:
  sea.print "only"
  sea.assume %t
  sea.br ^m
^m:
  sea.br ^j(%v : i8)
^b:
  %w = sea.nondet "w" [2] : i8
  sea.br ^j(%w : i8)
^j(%p: i8):
  %z = sea.const 3 : i8
  %e = sea.icmp ne, %p, %z : i8
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
