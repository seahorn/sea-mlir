
sea.program @assert_false_halt {
  %f = sea.const false
  sea.assert %f loc("f.c":21:3)
  sea.print "at end"
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
