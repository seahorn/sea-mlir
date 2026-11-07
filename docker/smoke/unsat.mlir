// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %zero = sea.const 0 : i8
  %memory = sea.mem.const %zero : !m
  %size = sea.const 16 : i64
  %object = sea.malloc %size align 16 [0] : i64 -> !sea.ptr<64>
  %value = sea.nondet "value" [1] : i8
  %written = sea.store %memory[%object], %value inplace : !m, i8
  %read = sea.load %written[%object] : !m -> i8
  %ok = sea.icmp eq, %read, %value : i8
  sea.assert %ok
  sea.halt
}
