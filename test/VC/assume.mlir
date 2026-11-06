// RUN: sea-vc %s --program=guarded | FileCheck %s --check-prefix=SAFE
// RUN: sea-vc %s --program=branch_assume | FileCheck %s --check-prefix=BUG

// An assume on the path rules the bug out.
// SAFE: result: unsat
sea.program @guarded {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %lt = sea.icmp ult, %x, %c100 : i8
  sea.assume %lt
  %y = sea.add %x, %c100 : i8
  %ok = sea.icmp uge, %y, %c100 : i8
  sea.assert %ok
  sea.halt
}

// An assume on a branch not taken does not constrain the other branch.
// BUG: result: sat
// BUG: replay: assert-failed
// BUG: validate: checked={{[0-9]+}} mismatches=0
sea.program @branch_assume {
  %x = sea.nondet "nondet_u8" [0] : i8
  %b = sea.nondet "nondet_bool" [1] : i1
  sea.cond_br %b, ^then, ^else
^then:
  %c0 = sea.const 0 : i8
  %nz = sea.icmp ne, %x, %c0 : i8
  sea.assume %nz
  sea.br ^join
^else:
  sea.br ^join
^join:
  %c0b = sea.const 0 : i8
  %ok = sea.icmp ne, %x, %c0b : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
