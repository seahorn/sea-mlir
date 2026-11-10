sea.program @main {
  %x = sea.nondet "x" [0] : i1
  sea.assume %x
  sea.unreachable
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
