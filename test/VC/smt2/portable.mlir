// REQUIRES: z3, yices-bin, bitwuzla-bin
// The --smt2 query read by z3, Yices and Bitwuzla (the command-line solvers
// of the pinned installs). Mul overflow is native for z3 (bvumul_noovfl,
// bvsmul_noovfl, bvsmul_noudfl: z3 only); --smt2-dialect=portable expands it
// exactly, and every solver gets the verdict of sea-vc.
// RUN: sea-vc %s --program=umul_sat --smt2 --smt2-dialect=portable > %t.us.smt2
// RUN: %z3 %t.us.smt2 | FileCheck %s --check-prefix=SAT
// RUN: %yices_smt2 %t.us.smt2 | FileCheck %s --check-prefix=SAT
// RUN: %bitwuzla %t.us.smt2 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=umul_unsat --smt2 --smt2-dialect=portable > %t.uu.smt2
// RUN: %z3 %t.uu.smt2 | FileCheck %s --check-prefix=UNSAT
// RUN: %yices_smt2 %t.uu.smt2 | FileCheck %s --check-prefix=UNSAT
// RUN: %bitwuzla %t.uu.smt2 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=smul_sat --smt2 --smt2-dialect=portable > %t.ss.smt2
// RUN: %z3 %t.ss.smt2 | FileCheck %s --check-prefix=SAT
// RUN: %yices_smt2 %t.ss.smt2 | FileCheck %s --check-prefix=SAT
// RUN: %bitwuzla %t.ss.smt2 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=smul_unsat --smt2 --smt2-dialect=portable > %t.su.smt2
// RUN: %z3 %t.su.smt2 | FileCheck %s --check-prefix=UNSAT
// RUN: %yices_smt2 %t.su.smt2 | FileCheck %s --check-prefix=UNSAT
// RUN: %bitwuzla %t.su.smt2 | FileCheck %s --check-prefix=UNSAT
// the verdicts of sea-vc itself
// RUN: sea-vc %s --program=umul_sat | FileCheck %s --check-prefix=RSAT
// RUN: sea-vc %s --program=umul_unsat | FileCheck %s --check-prefix=RUNSAT
// RUN: sea-vc %s --program=smul_sat | FileCheck %s --check-prefix=RSAT
// RUN: sea-vc %s --program=smul_unsat | FileCheck %s --check-prefix=RUNSAT
// SAT: {{^sat$}}
// UNSAT: {{^unsat$}}
// RSAT: result: sat
// RUNSAT: result: unsat

// The default dialect is z3's: the query z3 solves, its mul overflow named
// in the header and warned; z3 reads it
// RUN: sea-vc %s --program=smul_sat --smt2 > %t.z3.smt2 2> %t.z3.err
// RUN: FileCheck %s --check-prefix=Z3 < %t.z3.smt2
// RUN: FileCheck %s --check-prefix=Z3WARN < %t.z3.err
// RUN: %z3 %t.z3.smt2 | FileCheck %s --check-prefix=SAT
// Z3: ; sea.smt2-dialect z3
// Z3-DAG: ; sea.smt2-nonportable bvsmul_noovfl: read by z3 only; --smt2-dialect=portable expands it
// Z3-DAG: ; sea.smt2-nonportable bvsmul_noudfl: read by z3 only; --smt2-dialect=portable expands it
// Z3: (set-logic ALL)
// Z3: bvsmul_noovfl
// Z3WARN-DAG: [smt2.nonportable.bvsmul_noovfl]
// Z3WARN-DAG: [smt2.nonportable.bvsmul_noudfl]

// A query with nothing z3-only is read by all three in the default dialect
// (Yices needs the logic)
// RUN: sea-vc %s --program=plain --smt2 > %t.p.smt2 2> %t.p.err
// RUN: %z3 %t.p.smt2 | FileCheck %s --check-prefix=SAT
// RUN: %yices_smt2 %t.p.smt2 | FileCheck %s --check-prefix=SAT
// RUN: %bitwuzla %t.p.smt2 | FileCheck %s --check-prefix=SAT
// RUN: FileCheck %s --check-prefix=PLAIN < %t.p.smt2
// RUN: FileCheck %s --check-prefix=PLAINERR < %t.p.err
// PLAIN-NOT: sea.smt2-nonportable
// PLAINERR-NOT: smt2.nonportable

// no unsigned overflow happens for some x, y
sea.program @umul_sat {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %o = sea.umulo %x, %y : i8
  %t = sea.const 1 : i1
  %ok = sea.xor %o, %t : i1
  sea.assert %ok
  sea.halt
}

// x * y with y <= 1 never overflows
sea.program @umul_unsat {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %c2 = sea.const 2 : i8
  %small = sea.icmp ult, %y, %c2 : i8
  sea.assume %small
  %o = sea.umulo %x, %y : i8
  %t = sea.const 1 : i1
  %ok = sea.xor %o, %t : i1
  sea.assert %ok
  sea.halt
}

// no signed overflow happens for some x, y
sea.program @smul_sat {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %o = sea.smulo %x, %y : i8
  %t = sea.const 1 : i1
  %ok = sea.xor %o, %t : i1
  sea.assert %ok
  sea.halt
}

// x * y with y in [0, 1] never overflows, signed
sea.program @smul_unsat {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %c2 = sea.const 2 : i8
  %small = sea.icmp ult, %y, %c2 : i8
  sea.assume %small
  %o = sea.smulo %x, %y : i8
  %t = sea.const 1 : i1
  %ok = sea.xor %o, %t : i1
  sea.assert %ok
  sea.halt
}

// no z3-only construct
sea.program @plain {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c9 = sea.const 9 : i8
  %ok = sea.icmp ult, %x, %c9 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
