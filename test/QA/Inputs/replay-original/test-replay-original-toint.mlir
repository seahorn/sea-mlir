!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %ia = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %z = sea.const 0 : i64
  %ok = sea.icmp eq, %ia, %z : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
