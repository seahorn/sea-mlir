
sea.program @assume_true {
  %t = sea.const true
  sea.assume %t
  %x = sea.nondet "x" [0] : i1
  sea.assume %x
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
