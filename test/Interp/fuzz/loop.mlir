// A site has one value per trial, as with --input: a nondet read again in
// a loop gives the value of its first read (the assert would fail if a
// later read differed).
// RUN: sea-run %s --fuzz=500 --seed=1 | FileCheck %s

// CHECK-NOT: fuzz.failure
// CHECK: fuzz.status: halted=500 assert-failed=0
sea.program @main {
  %zero = sea.const 0 : i32
  sea.br ^loop(%zero, %zero : i32, i32)
^loop(%i : i32, %prev : i32):
  %x = sea.nondet "nondet_u32" [0] : i32
  %isfirst = sea.icmp eq, %i, %zero : i32
  %same = sea.icmp eq, %x, %prev : i32
  %ok = sea.or %isfirst, %same : i1
  sea.assert %ok
  %one = sea.const 1 : i32
  %next = sea.add %i, %one : i32
  %c3 = sea.const 3 : i32
  %more = sea.icmp ult, %next, %c3 : i32
  sea.cond_br %more, ^loop(%next, %x : i32, i32), ^done
^done:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
