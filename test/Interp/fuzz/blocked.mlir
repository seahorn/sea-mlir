// Per-assume blocking counts. The seeds run first, in file order, so the
// counts of a run of exactly the seeds are known: two seeds stop at the
// second assume, one at the first.
// RUN: rm -rf %t && mkdir -p %t
// RUN: echo '0=1' > %t/a.txt
// RUN: printf '0=5\n1=0\n' > %t/b.txt
// RUN: printf '0=6\n1=0\n' > %t/c.txt
// RUN: printf '0=7\n1=1\n' > %t/d.txt
// RUN: sea-run %s --fuzz=4 --corpus=%t | FileCheck %s

// CHECK: fuzz.trials: 4 seeds=4
// CHECK: fuzz.status: halted=1 assert-failed=0 assume-blocked=3 step-limit=0 error=0
// CHECK: fuzz.coverage: blocks=1 blocks-total=1 edges=1 assumes=2 assumes-total=2
// CHECK-NEXT: fuzz.corpus:
// CHECK-NEXT: fuzz.blocked: 2 op=sea.assume at={{.*}}blocked.mlir:[[@LINE+11]]
// CHECK-NEXT: fuzz.blocked: 1 op=sea.assume at={{.*}}blocked.mlir:[[@LINE+6]]
// CHECK-NOT: fuzz.
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c4 = sea.const 4 : i8
  %gt = sea.icmp ugt, %x, %c4 : i8
  sea.assume %gt
  %y = sea.nondet "nondet_u8" [1] : i8
  %c1 = sea.const 1 : i8
  %eq = sea.icmp eq, %y, %c1 : i8
  sea.assume %eq
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
