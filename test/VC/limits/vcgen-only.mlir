// --vcgen-only: the VC is generated and its statistics printed, no solver
// runs (no solve-time), and the result is `none`; --smt2 is refused.
// RUN: sea-vc %s --vcgen-only | FileCheck %s
// RUN: not sea-vc %s --vcgen-only --smt2 2>&1 | FileCheck %s --check-prefix=SMT2
// CHECK: vc-time:
// CHECK-NOT: solve-time:
// CHECK: result: none
// CHECK-NOT: replay:
// SMT2: --vcgen-only runs no solver
sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %y = sea.add %x, %c100 : i8
  %ok = sea.icmp uge, %y, %c100 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
