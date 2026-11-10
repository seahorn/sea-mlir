// An out-of-bounds load followed by an assume that blocks the run: with
// --defer-memory-errors the run is infeasible (assume-blocked, carrying
// one deferred violation); without it the run stops at the load.
// RUN: sea-run %s --input=2=9 --defer-memory-errors | FileCheck %s
// RUN: not sea-run %s --input=2=9 2>&1 | FileCheck %s --check-prefix=STOP
// RUN: sea-run %s --input=2=9 --defer-memory-errors --format=jsonl | FileCheck %s --check-prefix=JSON

// CHECK: status: assume-blocked
// CHECK-NEXT: at: loc({{.*}}blocked.mlir":[[@LINE+17]]
// CHECK-NEXT: deferred: 1 kind=out-of-bounds at={{.*}}blocked.mlir:[[@LINE+13]]
// CHECK-NOT: memory-error
// STOP: status: error
// STOP-NEXT: at: loc({{.*}}blocked.mlir":[[@LINE+10]]
// STOP-NEXT: message: memory access out of bounds
// JSON: "status":"assume-blocked",{{.*}}"deferred":1,"deferred_kind":"out-of-bounds"
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "nondet_u64" [2] : i64
  %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %v = sea.load %m0[%q] : !pm -> i8
  // a late assumption: the access above was on an infeasible run
  %ok = sea.icmp ult, %i, %n : i64
  sea.assume %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
