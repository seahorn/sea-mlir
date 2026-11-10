// A run that stops at an interpreter limit is a fuzz.limit, never a
// finding, with or without --defer-memory-errors: counted per op, with a
// warning (fuzz.limit, once); the fuzzer goes on and exits 0.
// RUN: rm -rf %t && sea-run %s --fuzz=300 --seed=1 --fuzz-out=%t 2>%t.err | FileCheck %s
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// RUN: ls %t | FileCheck %s --check-prefix=FILES
// RUN: sea-run %s --fuzz=300 --seed=1 --defer-memory-errors 2>&1 | FileCheck %s --check-prefix=DEFER

// CHECK-NOT: status:
// CHECK-NOT: fuzz.memory
// CHECK: fuzz.limit: {{[1-9][0-9]*}} trial={{[0-9]+}} file={{.*}}limit-{{[0-9]+}}.txt op=sea.memhavoc site=2 at={{.*}}limit.mlir:[[@LINE+17]]
// CHECK-NEXT: fuzz.limit-message: memhavoc of {{[0-9]+}} cells exceeds the interpreter limit
// WARN: Warning: a trial stopped at an interpreter limit {{.*}}[fuzz.limit]
// FILES: limit-
// DEFER-NOT: fuzz.memory-safety
// DEFER: fuzz.limit: {{[1-9][0-9]*}} trial=
// DEFER: fuzz.status: halted={{[0-9]+}} assert-failed=0 assume-blocked=0 step-limit=0 error={{[1-9][0-9]*}} memory-error=0
// DEFER-NEXT: fuzz.deferred: 0 memory-error=0
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !pm
  // a large object: a long memhavoc is in bounds, past the interpreter's
  // length limit
  %big = sea.const 0x10000000000 : i64
  %p = sea.malloc %big align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "nondet_u64" [3] : i64
  %len = sea.umin %n, %big : i64
  %m1 = sea.memhavoc %m0[%p], %len [2] inplace : !pm
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
