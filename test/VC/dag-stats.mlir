// --dag-stats: the query's DAG size and distinct nodes per operator.
// RUN: sea-vc %s --dag-stats | FileCheck %s
// CHECK: vc.dag-size: {{[1-9][0-9]*}}
// CHECK: vc.dag-op: bvadd 1
// CHECK: result: unsat
sea.program @p {
  %x = sea.nondet "x" [0] : i8
  %y = sea.add %x, %x : i8
  %z = sea.add %x, %x : i8
  %e = sea.icmp eq, %y, %z : i8
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
