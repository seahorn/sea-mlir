sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %z = sea.const 0 : i8
  %c = sea.icmp eq, %x, %z : i8
  sea.cond_br %c, ^err, ^ok
^ok:
  sea.halt
^err:
  %f = sea.const 0 : i1
  sea.print "at error"
  sea.assert %f
  sea.print "after"
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
