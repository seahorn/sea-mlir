// An unwritable --fuzz-coverage-out fails at once, before any trial.
// RUN: not sea-run %s --program=safe --fuzz=100000000 --seed=1 --fuzz-coverage-out=/nonexistent-dir/cov.txt 2>&1 | FileCheck %s
// CHECK: cannot write '/nonexistent-dir/cov.txt'
// CHECK-NOT: fuzz.trials
sea.program @safe {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assume %lt
  %c5 = sea.const 5 : i8
  %small = sea.icmp ult, %x, %c5 : i8
  sea.cond_br %small, ^a, ^b
^a:
  sea.br ^done
^b:
  sea.br ^done
^done:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
