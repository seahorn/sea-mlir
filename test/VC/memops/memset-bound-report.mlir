// assume.memset-bound: --lower without --seam expands a symbolic-length
// memset into a bounded chain with assume(n <= K) (sea-expand-memset); the
// result block reports it. --seam encodes the memset exactly: no line.
// Inputs: the expand-memset test program and the over-bound one.
// RUN: sea-vc %S/../../Transforms/expand-memset.mlir --lower | FileCheck %s
// RUN: sea-vc %S/memset-over-bound.mlir --program=over_bound --lower 2>&1 | FileCheck %s --check-prefix=OVER
// RUN: sea-vc %S/memset-over-bound.mlir --program=over_bound --lower --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// CHECK: assume.memset-bound: count=1 bound=16
// CHECK: result: sat
// OVER: assume.memset-bound: count=1 bound=16
// OVER: result: unsat
// SEAM-NOT: assume.memset-bound
// SEAM: result: sat

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
