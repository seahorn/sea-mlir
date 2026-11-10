sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %c = sea.nondet "c" [1] : i1
  sea.assume %c
  sea.unreachable
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
