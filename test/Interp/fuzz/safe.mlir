// A safe program: every trial runs clean, coverage reaches every block;
// the same seed gives the same run (the timing line aside).
// RUN: sea-run %s --fuzz=3000 --seed=7 > %t.1
// RUN: FileCheck %s < %t.1
// RUN: sea-run %s --fuzz=3000 --seed=7 > %t.2
// RUN: grep -v '^fuzz.trials' %t.1 > %t.1.s
// RUN: grep -v '^fuzz.trials' %t.2 > %t.2.s
// RUN: diff %t.1.s %t.2.s
// RUN: sea-run %s --fuzz=3000 --seed=7 --format=jsonl | FileCheck %s --check-prefix=JSONL

// CHECK-NOT: status:
// CHECK: fuzz.trials: 3000 seeds=0
// CHECK: fuzz.status: halted={{[0-9]+}} assert-failed=0 assume-blocked={{[0-9]+}} step-limit=0 error=0
// CHECK: fuzz.coverage: blocks=4 blocks-total=4 edges=5 assumes=1 assumes-total=1
// CHECK-NOT: fuzz.failure
// JSONL: {"kind":"stat","key":"fuzz.coverage","blocks":4,"blocks-total":4,
// JSONL: {"kind":"stat","key":"fuzz.blocked","value":{{[0-9]+}},"op":"sea.assume","at":
sea.program @main {
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
  %c20 = sea.const 20 : i8
  %ok = sea.icmp ult, %x, %c20 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
