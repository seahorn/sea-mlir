sea.program @main {
  %a = sea.nondet "nd" [0] : i8
  %c1 = sea.const 1 : i8
  %x = sea.icmp ne, %a, %c1 : i8
  sea.assert %x
  %z = sea.icmp ult, %a, %c1 : i8
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
