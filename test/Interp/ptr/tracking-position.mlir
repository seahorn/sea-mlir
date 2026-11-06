// RUN: not sea-opt %s --sea-apply-tracking -o /dev/null 2>&1 | FileCheck %s
// RUN: not sea-opt %s --sea-apply-tracking --split-input-file 2>&1 | FileCheck %s --check-prefix=EARLY

// A marker on one branch only, with an update after the join, is not at a
// dominating position: whether the update is tracked would depend on the
// path.
// CHECK: is not on every path to a memory update after it{{.*}}[tracking.position]
sea.program @conditional {
  %m0 = sea.mem.nondet "m" [1] : !sea.mem<!sea.ptr<64>, i8>
  %n = sea.const 8 : i64
  %a = sea.alloca %n align 8 [2] : i64 -> !sea.ptr<64>
  %v = sea.const 1 : i8
  %c = sea.nondet "c" [0] : i1
  sea.cond_br %c, ^on, ^join
^on:
  sea.tracking_on
  sea.br ^join
^join:
  %m1 = sea.store %m0[%a], %v inplace : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.halt
}

// -----

// A path that ends early (a failed assumption) may bypass the marker: it
// reaches no update. The store after the marker is tracked.
// EARLY-LABEL: sea.program @early_exit
// EARLY: sea.store {{.*}} inplace tracked
sea.program @early_exit {
  %m0 = sea.mem.nondet "m" [1] : !sea.mem<!sea.ptr<64>, i8>
  %n = sea.const 8 : i64
  %a = sea.alloca %n align 8 [2] : i64 -> !sea.ptr<64>
  %v = sea.const 1 : i8
  %c = sea.nondet "c" [0] : i1
  sea.cond_br %c, ^go, ^end
^go:
  sea.tracking_on
  %m1 = sea.store %m0[%a], %v inplace : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.br ^end
^end:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
