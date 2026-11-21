// RUN: sea-opt %s --sea-split-asserts --split-input-file 2>/dev/null | FileCheck %s
// RUN: sea-opt %s --sea-split-asserts --split-input-file 2>&1 >/dev/null | FileCheck %s --check-prefix=WARN
// RUN: sea-opt %s --sea-split-asserts=program=other --split-input-file 2>&1 | FileCheck %s --check-prefix=ONLY

// Three asserts: a0 in the entry, a1 on the left branch, a2 at the join.
// Part i keeps a_i; every other assert in its cone is an assume; the cone
// is a_i's block and its ancestors, the block ends at a_i with a halt.
// CHECK-LABEL: sea.program @p.assert0
// CHECK-SAME: sea.split_assert = {count = 3 : i64, index = 0 : i64}
// CHECK-NEXT: sea.nondet
// CHECK-NEXT: sea.nondet
// CHECK-NEXT: sea.assert %{{.*}} {tag = 0 : i64}
// CHECK-NEXT: sea.halt
// CHECK-NEXT: }
// The left branch only: the cond_br becomes an assume of its condition.
// CHECK-LABEL: sea.program @p.assert1
// CHECK: sea.assume %[[A:.*]] {tag = 0 : i64}
// CHECK-NEXT: sea.assume %[[C:.*]]
// CHECK-NEXT: sea.br ^[[L:.*]]
// CHECK: ^[[L]]:
// CHECK-NEXT: sea.nondet "b"
// CHECK-NEXT: sea.assert %{{.*}} {tag = 1 : i64}
// CHECK-NEXT: sea.halt
// CHECK-NOT: ^bb
// CHECK: }
// The join: both branches, both earlier asserts assumed.
// CHECK-LABEL: sea.program @p.assert2
// CHECK: sea.assume %{{.*}} {tag = 0 : i64}
// CHECK: sea.cond_br
// CHECK: sea.assume %{{.*}} {tag = 1 : i64}
// CHECK: sea.assert %{{.*}} {tag = 2 : i64}
// CHECK-NEXT: sea.halt
// CHECK-NOT: sea.program @p {
// ONLY: sea.program @p {
sea.program @p {
  %a = sea.nondet "a" [0] : i1
  %c = sea.nondet "c" [1] : i1
  sea.assert %a {tag = 0 : i64}
  sea.cond_br %c, ^l, ^r
^l:
  %b = sea.nondet "b" [2] : i1
  sea.assert %b {tag = 1 : i64}
  sea.br ^j
^r:
  sea.br ^j
^j:
  %d = sea.nondet "d" [3] : i1
  sea.assert %d {tag = 2 : i64}
  sea.assume %d
  sea.halt
}

// -----

// The false edge leads to the assert: the assume negates the condition.
// CHECK-LABEL: sea.program @neg.assert0
// CHECK: %[[C:.*]] = sea.nondet "c"
// CHECK: %[[T:.*]] = sea.const true
// CHECK: %[[N:.*]] = sea.xor %[[C]], %[[T]] : i1
// CHECK: sea.assume %[[N]]
// CHECK: sea.assert
sea.program @neg {
  %c = sea.nondet "c" [0] : i1
  sea.cond_br %c, ^t, ^f
^t:
  sea.halt
^f:
  %b = sea.nondet "b" [1] : i1
  sea.assert %b
  sea.halt
}

// -----

// An assert no path reaches: its part cannot fail.
// WARN: Warning: {{.*}}[split-asserts.unreachable]
// CHECK-LABEL: sea.program @dead.assert0
// CHECK-NEXT: %[[T:.*]] = sea.const true
// CHECK-NEXT: sea.assert %[[T]]
// CHECK-NEXT: sea.halt
sea.program @dead {
  sea.halt
^x:
  %b = sea.nondet "b" [0] : i1
  sea.assert %b
  sea.halt
}

// -----

// A program without asserts stays as it is.
// CHECK-LABEL: sea.program @none
// CHECK-NOT: sea.split_assert
sea.program @none {
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
