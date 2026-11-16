sea.program @main {
  %t = sea.const 1 : i1
  sea.tracking_on loc("on")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
