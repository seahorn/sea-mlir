sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %f = sea.const 0 : i1
  sea.assert %f loc("check.c":5:3)
  %z = sea.const 0 : i8
  %nz = sea.icmp ne, %x, %z : i8
  sea.assume %nz
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
