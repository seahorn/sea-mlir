// The A-ALIGN discharge job (sea-vc --check-align; sea-check-align then
// sea-merge-asserts): every `align` promise of an access is asserted after
// placement, the program's own asserts are dropped. @good: an 8-byte load
// with align 8 from an 8-aligned allocation: unsat (the promise holds).
// @bad: the same load at offset 4 still promises align 8: sat, and the
// replay fails at the merged assert (an execution breaks the promise).
// The functional assert (x = x + 1, false) is not checked by this job.
// RUN: sea-vc %s --program=good --lower --check-align | FileCheck %s --check-prefix=GOOD
// RUN: sea-vc %s --program=bad --lower --check-align | FileCheck %s --check-prefix=BAD
// RUN: sea-vc %s --program=good --lower | FileCheck %s --check-prefix=FUNC
// GOOD: result: unsat
// BAD: result: sat
// BAD: replay: assert-failed
// FUNC: result: sat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @good {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %x = sea.load %m0[%a] align 8 : !m -> i64
  %one = sea.const 1 : i64
  %y = sea.add %x, %one : i64
  %ok = sea.icmp eq, %x, %y : i64
  sea.assert %ok
  sea.halt
}
sea.program @bad {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %c4 = sea.const 4 : i64
  %p = sea.ptr.add %a, %c4 : !sea.ptr<64>, i64
  %x = sea.load %m0[%p] align 8 : !m -> i64
  %ok = sea.icmp eq, %x, %x : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
