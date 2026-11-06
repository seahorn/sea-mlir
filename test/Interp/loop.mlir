// RUN: sea-run %s --program=count | FileCheck %s --check-prefix=COUNT
// RUN: sea-run %s --program=forever --max-steps=50 | FileCheck %s --check-prefix=FOREVER

// The interpreter does not require a loop-free CFG; block arguments are bound
// as a parallel copy (the swap below).
// COUNT: status: halted
sea.program @count {
  %z = sea.const 0 : i8
  %o = sea.const 1 : i8
  %t = sea.const 2 : i8
  sea.br ^loop(%z, %o, %t : i8, i8, i8)
^loop(%i : i8, %p : i8, %q : i8):
  %c3 = sea.const 3 : i8
  %done = sea.icmp eq, %i, %c3 : i8
  %i1 = sea.add %i, %o : i8
  sea.cond_br %done, ^exit(%p : i8), ^loop(%i1, %q, %p : i8, i8, i8)
^exit(%r : i8):
  // Three swaps of (1, 2): p ends as 2.
  %ok = sea.icmp eq, %r, %t : i8
  sea.assert %ok
  sea.halt
}

// FOREVER: status: step-limit
// FOREVER: steps: 51
sea.program @forever {
  sea.br ^loop
^loop:
  sea.br ^loop
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
