sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %z = sea.const 0 : i8
  %c1 = sea.icmp ne, %x, %z : i8
  sea.assert %c1 loc("first.mlir":3:3)
  %t = sea.const true
  %f = sea.xor %t, %t : i1
  sea.assume %f
  %c2 = sea.icmp ne, %x, %z : i8
  sea.assert %c2 loc("second.mlir":4:3)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
