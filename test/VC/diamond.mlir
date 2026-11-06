// RUN: sea-vc %s --program=bug | FileCheck %s --check-prefix=BUG
// RUN: %if yices %{ sea-vc %s --program=bug --solver=yices | FileCheck %s --check-prefix=BUG %}
// RUN: sea-vc %s --program=safe | FileCheck %s --check-prefix=SAFE
// RUN: %if yices %{ sea-vc %s --program=safe --solver=yices | FileCheck %s --check-prefix=SAFE %}

// y = x < 10 ? x + x : 1; assert y < 18 fails only for x = 9.
// BUG: result: sat
// BUG: nondet: site=0 callee=nondet_u8 width=8 value=9
// BUG: replay: assert-failed
// BUG: validate: checked={{[0-9]+}} mismatches=0
sea.program @bug {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %small = sea.icmp ult, %x, %c10 : i8
  sea.cond_br %small, ^then, ^else
^then:
  %d = sea.add %x, %x : i8
  sea.br ^join(%d : i8)
^else:
  %c1 = sea.const 1 : i8
  sea.br ^join(%c1 : i8)
^join(%y : i8):
  %c18 = sea.const 18 : i8
  %ok = sea.icmp ult, %y, %c18 : i8
  sea.assert %ok
  sea.halt
}

// SAFE: result: unsat
sea.program @safe {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %small = sea.icmp ult, %x, %c10 : i8
  sea.cond_br %small, ^then, ^else
^then:
  %d = sea.add %x, %x : i8
  sea.br ^join(%d : i8)
^else:
  %c1 = sea.const 1 : i8
  sea.br ^join(%c1 : i8)
^join(%y : i8):
  %c19 = sea.const 19 : i8
  %ok = sea.icmp ult, %y, %c19 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
