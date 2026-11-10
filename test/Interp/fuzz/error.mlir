// A run that stops with an error (an access out of its object) is recorded
// per op and does not stop the fuzzer; its first input replays to the
// same error.
// RUN: rm -rf %t && sea-run %s --fuzz=500 --seed=3 --fuzz-out=%t | FileCheck %s
// RUN: cat %t/error-*.txt > %t.err
// RUN: not sea-run %s --input-file=%t.err 2>&1 | FileCheck %s --check-prefix=REPLAY

// CHECK-NOT: status:
// CHECK: fuzz.error: {{[0-9]+}} trial={{[0-9]+}} file={{.*}}error-{{[0-9]+}}.txt op=sea.load at={{.*}}error.mlir:[[@LINE+13]]
// CHECK-NEXT: fuzz.error-message: {{.*}}out of bounds
// CHECK: fuzz.trials: 500
// CHECK: fuzz.status: halted={{[1-9][0-9]*}} assert-failed=0 assume-blocked=0 step-limit=0 error={{[1-9][0-9]*}}
// REPLAY: status: error
// REPLAY-NEXT: at: loc({{.*}}error.mlir":[[@LINE+8]]
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "nondet_u64" [2] : i64
  %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %v = sea.load %m0[%q] : !pm -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
