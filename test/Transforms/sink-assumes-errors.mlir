// RUN: not sea-opt %s --sea-sink-assumes 2>&1 | FileCheck %s

// Not BMC form (two asserts): the pass refuses.
// CHECK: sea-sink-assumes.form
sea.program @two_asserts {
  %b = sea.nondet "nondet_bool" [0] : i1
  sea.assume %b
  sea.assert %b
  sea.assert %b
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
