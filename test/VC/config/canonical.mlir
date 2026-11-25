// The shipped canonical profile (profiles/canonical.cfg) loads and gives the
// verdict of the plain options. It composes with seam (no --lower / --seam
// in it) and sets the z3 tactic.
// RUN: env -u SEA_MLIR_PROFILE_PATH sea-vc %s --config=canonical | FileCheck %s --check-prefix=PROF
// RUN: sea-vc %s --solver-nary-sums --vc-canonical-sums --vc-canonical-sums-neg=auto --vc-canonical-sums-eq=unshared --vc-canonical-sums-inner=flatten | FileCheck %s --check-prefix=PLAIN
// RUN: env -u SEA_MLIR_PROFILE_PATH sea-vc %s --config=seam --config=canonical | FileCheck %s --check-prefix=SEAM
// PROF: vc.config: {{.*}}canonical.cfg
// PROF: vc.canonical-sums: on neg=bvnot neg-choice=auto eq=unshared inner=flatten
// PROF: solver.z3-tactic: (using-params (then (using-params simplify :som true
// PROF: result: sat
// PLAIN: vc.canonical-sums: on neg=bvnot neg-choice=auto eq=unshared inner=flatten
// PLAIN: result: sat
// SEAM: vc.config: {{.*}}seam.cfg
// SEAM: vc.config: {{.*}}canonical.cfg
// SEAM: vc.canonical-sums: on
// SEAM: result: sat
sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %c100 = sea.const 100 : i8
  %s = sea.add %x, %y : i8
  %t = sea.add %s, %c100 : i8
  %ok = sea.icmp uge, %t, %c100 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
