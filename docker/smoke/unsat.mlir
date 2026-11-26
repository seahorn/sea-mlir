// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %zero = sea.const 0 : i8
  %memory = sea.mem.const %zero : !m
  %size = sea.const 16 : i64
  %object = sea.malloc %size align 16 [0] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [1] : i64
  %j = sea.nondet "j" [2] : i64
  %value = sea.nondet "value" [3] : i8
  %same = sea.icmp eq, %i, %j : i64
  sea.assume %same
  %inside = sea.icmp ult, %i, %size : i64
  sea.assume %inside
  %at_i = sea.ptr.add %object, %i : !sea.ptr<64>, i64
  %at_j = sea.ptr.add %object, %j : !sea.ptr<64>, i64
  %written = sea.store %memory[%at_i], %value inplace : !m, i8
  %read = sea.load %written[%at_j] : !m -> i8
  %ok = sea.icmp eq, %read, %value : i8
  sea.assert %ok
  sea.halt
}
