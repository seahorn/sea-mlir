!am = !sea.mem<!sea.addr<16>, i8>
sea.program @main {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
