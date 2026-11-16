!pm = !sea.mem<!sea.ptr<8>, i8>
sea.program @main {
  %z = sea.const 0 : i8 loc("z")
  %m0 = sea.mem.const %z : !pm loc("m0")
  %n = sea.const 8 : i8 loc("n")
  %p = sea.malloc %n align 1 [1] : i8 -> !sea.ptr<8> loc("p")
  %b = sea.mem.is_modified %m0[%p] : !pm loc("b")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
