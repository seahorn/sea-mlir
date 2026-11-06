// A sea-vc built without Yices (SEA_ENABLE_YICES=OFF or not found) refuses
// --solver=yices and the --yices-* options before doing any work.
// UNSUPPORTED: yices
// RUN: not sea-vc %s --solver=yices 2>&1 | FileCheck %s
// RUN: not sea-vc %s --yices-param=random-seed=7 2>&1 | FileCheck %s
// RUN: not sea-vc %s --solver=z3 --yices-logic=QF_BV 2>&1 | FileCheck %s
// RUN: sea-vc --version | FileCheck %s --check-prefix=VERSION
// RUN: sea-vc %s | FileCheck %s --check-prefix=Z3
// CHECK: sea-vc: this sea-vc was built without Yices: --solver=yices and --yices-* are unavailable
// VERSION: yices: not built (--solver=yices disabled)
// Z3: result: unsat

sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.and %x, %x : i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
