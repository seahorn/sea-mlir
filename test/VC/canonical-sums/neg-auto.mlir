// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-neg=auto --smt2 2>%t.z3 | tr '\n' ' ' | tr -s ' ' | FileCheck %s --check-prefix=BVNOT --implicit-check-not=bvsub
// RUN: FileCheck %s --check-prefix=STAT-BVNOT < %t.z3
// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-neg=auto --solver=z3 | FileCheck %s --check-prefix=STAT-BVNOT
// RUN: %if yices %{ sea-vc %s --vc-canonical-sums --vc-canonical-sums-neg=auto --solver=yices | FileCheck %s --check-prefix=STAT-SUB %}
// RUN: %if bitwuzla %{ sea-vc %s --vc-canonical-sums --vc-canonical-sums-neg=auto --solver=bitwuzla --smt2 2>%t.bz | tr '\n' ' ' | tr -s ' ' | FileCheck %s --check-prefix=BZNOT --implicit-check-not=bvsub %}
// RUN: %if bitwuzla %{ FileCheck %s --check-prefix=STAT-BVNOT < %t.bz %}
// The parts of --check-inb (Yices by default) get the option and resolve
// it with their own solver.
// RUN: rm -rf %t.d
// RUN: %if yices %{ sea-vc %S/../check-inb-solver.mlir --check-inb --lower --seam --ackermannize=off --vcgen-only --vc-canonical-sums --vc-canonical-sums-neg=auto --check-inb-dir=%t.d %}
// RUN: %if yices %{ FileCheck %s --check-prefix=CFG < %t.d/parts.cfg %}
// RUN: %if yices %{ cd %t.d && sea-vc part-0.mlir --config=parts.cfg | FileCheck %s --check-prefix=STAT-SUB %}

// --vc-canonical-sums-neg=auto (docs/design/CanonicalSums.md, "The
// negation"): the form follows the solver the query goes to, also for
// --smt2: sub for Yices, bvnot for z3 and Bitwuzla.
// BVNOT: (bvadd nd.0_0 (bvnot (bvadd nd.1_0 nd.2_0)) #x06)
// Bitwuzla's printer writes binary numerals and nested sums.
// BZNOT: (bvnot (bvadd nd.1_0 nd.2_0))
// STAT-BVNOT: vc.canonical-sums: on neg=bvnot neg-choice=auto
// STAT-SUB: vc.canonical-sums: on neg=sub neg-choice=auto
// CFG: --vc-canonical-sums-neg=auto
// CFG: --solver=yices
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %y = sea.nondet "f" [1] : i8
  %z = sea.nondet "f" [2] : i8
  %w = sea.nondet "f" [3] : i8
  %c5 = sea.const 5 : i8
  %c7 = sea.const 7 : i8
  %c200 = sea.const 200 : i8
  %c9 = sea.const 9 : i8
  %a = sea.sub %x, %y : i8
  %b = sea.sub %a, %z : i8
  %s = sea.add %b, %c5 : i8
  %m = sea.mul %c200, %w : i8
  %t = sea.add %x, %m : i8
  %u = sea.icmp ult, %t, %c9 : i8
  sea.assume %u
  %ok = sea.icmp ult, %s, %c7 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
