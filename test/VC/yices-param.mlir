// The Yices knobs choose how Yices searches, never what the verdict is.
// REQUIRES: yices
// RUN: sea-vc %s --program=bug --solver=yices --yices-param=random-seed=7 | FileCheck %s --check-prefix=SEED
// RUN: sea-vc %s --program=bug --solver=yices --yices-logic=QF_BV --yices-config=mode=one-shot | FileCheck %s --check-prefix=LOGIC
// RUN: sea-vc %s --program=safe --solver=yices --yices-param=random-seed=3,randomness=0.5 | FileCheck %s --check-prefix=SAFE
// RUN: not sea-vc %s --program=bug --solver=yices --yices-param=no-such-param=1 2>&1 | FileCheck %s --check-prefix=BADPARAM
// RUN: not sea-vc %s --program=bug --solver=yices --yices-config=no-such-key=1 2>&1 | FileCheck %s --check-prefix=BADCONFIG
// RUN: not sea-vc %s --program=bug --solver=yices --yices-param=random-seed 2>&1 | FileCheck %s --check-prefix=NOVALUE

// SEED: solver.yices-param: random-seed=7
// SEED: result: sat
// SEED: validate: checked={{[0-9]+}} mismatches=0
// LOGIC: solver.yices-logic: QF_BV
// LOGIC: solver.yices-config: mode=one-shot
// LOGIC: result: sat
// LOGIC: validate: checked={{[0-9]+}} mismatches=0
// SAFE: solver.yices-param: random-seed=3
// SAFE: solver.yices-param: randomness=0.5
// SAFE: result: unsat
// BADPARAM: vc.yices-param
// BADCONFIG: vc.yices-config
// NOVALUE: wants key=value
sea.program @bug {
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

sea.program @safe {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %lt = sea.icmp ult, %x, %c100 : i8
  sea.assume %lt
  %y = sea.add %x, %c100 : i8
  %ok = sea.icmp uge, %y, %c100 : i8
  sea.assert %ok
  sea.halt
}

// REQUIRES: yices

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
