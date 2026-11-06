// RUN: sea-vc %s | FileCheck %s --check-prefix=OK
// RUN: not sea-vc %s --vc-typecheck-plant 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not sea-vc %s --vc-typecheck-plant --lower --seam 2>&1 | FileCheck %s --check-prefix=BAD

// --vc-typecheck (default on; PtrSort rev 5, S0): the whole query is
// type-checked before the solver. A well-typed query passes (and the time
// is printed); an ill-typed one (planted by the hidden test hook: an 8-bit
// and a 16-bit numeral compared) is the error vc.ill-typed, not a solver
// crash.
// OK: vc.typecheck-time:
// OK: result: unsat
// BAD: vc.ill-typed
sea.program @ok {
  %x = sea.nondet "x" [0] : i8
  %y = sea.xor %x, %x : i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %y, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
