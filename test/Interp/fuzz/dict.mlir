// An assume(x == 1234) guard: passed through the dictionary (the
// program's constants), never by random values.
// RUN: sea-run %s --fuzz=2000 --seed=1 | FileCheck %s
// RUN: sea-run %s --fuzz=2000 --seed=1 --fuzz-dict=false | FileCheck %s --check-prefix=NODICT

// CHECK: nondet: site=0 callee=nondet_u32 width=32 value=1234
// CHECK: status: assert-failed
// CHECK: fuzz.failure: 1 trial=
// CHECK: fuzz.status: {{.*}} assert-failed=1
// CHECK: fuzz.coverage: blocks=1 blocks-total=1 edges=1 assumes=1 assumes-total=1
// NODICT-NOT: status:
// NODICT: fuzz.trials: 2000
// NODICT: fuzz.status: halted=0 assert-failed=0 assume-blocked=2000
// NODICT: fuzz.coverage: blocks=1 blocks-total=1 edges=1 assumes=0 assumes-total=1
// NODICT: fuzz.corpus: {{[0-9]+}} {{.*}} dictionary=0
// NODICT: fuzz.blocked: 2000 op=sea.assume
// NODICT-NOT: fuzz.failure
sea.program @main {
  %x = sea.nondet "nondet_u32" [0] : i32
  %c = sea.const 1234 : i32
  %eq = sea.icmp eq, %x, %c : i32
  sea.assume %eq
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
