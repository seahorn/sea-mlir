sea.program @main {
  %a = sea.nondet "nd" [0] : i8
  sea.bogus %a
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
