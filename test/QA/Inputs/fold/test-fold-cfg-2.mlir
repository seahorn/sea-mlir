
sea.program @assume_false_dead {
  sea.halt
^dead:
  %f = sea.const false
  sea.assume %f loc("f.c":11:3)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
