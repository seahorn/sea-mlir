// Splits on block guards, nested: one level per target in encoding order,
// each with its cases under the cases before it. y = x < 10 ? x + x : 1;
// assert y < 18 fails only for x = 9 (VC/diamond.mlir). ^bb1 is the then
// block, ^bb2 the else block: of the four parts only then=true, else=false
// is sat (the level order is the encoding's) (the two guards exclude each other; the exit must run).
// RUN: sea-vc %s --program=bug --split=^bb1,^bb2 | FileCheck %s --check-prefix=NEST
// NEST-DAG: split.check: level={{[01]}} target=^bb1 cases=2 exhaustive=ok exact=ok
// NEST-DAG: split.check: level={{[01]}} target=^bb2 cases=2 exhaustive=ok exact=ok
// NEST-DAG: choice={{(\^bb1=true#0/2,\^bb2=false#1/2|\^bb2=false#1/2,\^bb1=true#0/2)}} result=sat
// NEST-DAG: replay: assert-failed
// NEST: split.verdict: sat parts=4 unsat=3 sat=1 unknown=0
// NEST: result: sat

// The hard cap on the parts: an error, no verdict.
// RUN: not sea-vc %s --program=bug --split=^bb1,^bb2 --split-max-parts=3 2>&1 | FileCheck %s --check-prefix=CAP
// CAP: [vc.split-max-parts]
// CAP-NOT: split.verdict

// Each part's query as SMT-LIB, without solving (--vcgen-only); the parts
// answer as the split does.
// RUN: rm -rf %t && mkdir -p %t
// RUN: sea-vc %s --program=bug --split=^bb1 --split-smt2=%t/q --vcgen-only | FileCheck %s --check-prefix=SMT
// RUN: %z3 %t/q.part0.smt2 | FileCheck %s --check-prefix=Z3SAT
// RUN: %z3 %t/q.part1.smt2 | FileCheck %s --check-prefix=Z3UNSAT
// SMT: split.smt2: {{.*}}q.part0.smt2
// SMT: split.part: 0 {{.*}} result=none
// SMT: split.smt2: {{.*}}q.part1.smt2
// SMT: split.verdict: none parts=2
// SMT: result: none
// Z3SAT: {{^}}sat
// Z3UNSAT: {{^}}unsat

// Selectors that find nothing, or the wrong kind, and options that need a
// split, are errors.
// RUN: not sea-vc %s --program=bug --split=%%nope 2>&1 | FileCheck %s --check-prefix=NOPE
// RUN: not sea-vc %s --program=bug --split=^bb0 2>&1 | FileCheck %s --check-prefix=ENTRY
// RUN: not sea-vc %s --program=bug --split=nd.0 2>&1 | FileCheck %s --check-prefix=KIND
// RUN: not sea-vc %s --program=bug --split-max-parts=3 2>&1 | FileCheck %s --check-prefix=NEED
// RUN: not sea-vc %s --program=bug --split=^bb1 --smt2 2>&1 | FileCheck %s --check-prefix=SMT2
// NOPE: [vc.split-target]
// ENTRY: entry block
// KIND: not i1
// NEED: need --split, --split-leaves or --split-auto
// SMT2: not with --smt2

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

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
