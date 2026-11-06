// RUN: sea-vc %s | FileCheck %s --check-prefix=VC
// RUN: sea-run %s --input=0=1 | FileCheck %s --check-prefix=INTERP

// The branch into sea.unreachable is infeasible: only c = 0 reaches the
// exit, where the assertion fails.
// VC: result: sat
// VC: nondet: site=0 callee=nondet_bool width=1 value=0
// VC: replay: assert-failed
// INTERP: status: assume-blocked
sea.program @main {
  %c = sea.nondet "nondet_bool" [0] : i1
  sea.cond_br %c, ^dead, ^exit
^dead:
  sea.unreachable
^exit:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
