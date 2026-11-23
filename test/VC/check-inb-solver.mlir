// sea-vc --check-inb: the parts' solver (docs/design/CheckInb.md, "The
// parts' solver"). Yices when the binary has it, else z3 (said on the
// check-inb.solver line); an explicit --solver= wins; with z3,
// --ro-arrays=uf unless --ro-arrays= is given. The chosen options are in
// parts.cfg.
// RUN: rm -rf %t.d %t.z %t.s %t.r
// RUN: %if yices %{ sea-vc %s --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=DEF-Y %}
// RUN: %if !yices %{ sea-vc %s --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=DEF-Z %}
// RUN: %if yices %{ sea-vc %s --check-inb --lower --seam --ackermannize=off --vcgen-only --check-inb-dir=%t.d | FileCheck %s --check-prefix=PASS %}
// RUN: %if yices %{ FileCheck %s --check-prefix=CFG-Y < %t.d/parts.cfg %}
// An explicit --solver=z3 wins; z3 gets --ro-arrays=uf.
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --solver=z3 | FileCheck %s --check-prefix=Z3
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --solver=z3 --vcgen-only --check-inb-dir=%t.z | FileCheck %s --check-prefix=Z3
// RUN: FileCheck %s --check-prefix=CFG-Z < %t.z/parts.cfg
// An explicit --ro-arrays= is kept.
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --solver=z3 --ro-arrays=array --vcgen-only --check-inb-dir=%t.r | FileCheck %s --check-prefix=ROEXP
// RUN: FileCheck %s --check-prefix=CFG-R < %t.r/parts.cfg
// An explicit --solver=bitwuzla (when built) is passed through, no --ro-arrays.
// RUN: %if bitwuzla %{ sea-vc %s --check-inb --lower --seam --ackermannize=off --solver=bitwuzla --vcgen-only --check-inb-dir=%t.s | FileCheck %s --check-prefix=BZ %}
// RUN: %if bitwuzla %{ FileCheck %s --check-prefix=CFG-B < %t.s/parts.cfg %}
// A z3-only option without --solver keeps z3 for the parts, with its own
// reason; uf applies as usual.
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --z3-logic=QF_BV | FileCheck %s --check-prefix=LOGIC
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --z3-param=verbose=0 | FileCheck %s --check-prefix=PARAM
// With an explicit non-z3 solver the option is ignored: one warning at the job.
// RUN: %if yices %{ sea-vc %s --check-inb --lower --seam --ackermannize=off --solver=yices --z3-tactic=smtfd 2>&1 | FileCheck %s --check-prefix=IGN %}
// A neutral value (the option's default) does not count: Yices stays.
// RUN: %if yices %{ sea-vc %s --check-inb --lower --seam --ackermannize=off --z3-tactic=default | FileCheck %s --check-prefix=NEUTRAL %}
// RUN: %if yices %{ sea-vc %s --check-inb --lower --seam --ackermannize=off --seam-check-rlimit=0 | FileCheck %s --check-prefix=NEUTRAL %}
// The stat in jsonl.
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --solver=z3 --format=jsonl | FileCheck %s --check-prefix=JSON

// DEF-Y: check-inb.solver: yices (default) ro-arrays=array (default)
// DEF-Y: result: sat
// DEF-Z: check-inb.solver: z3 (no yices in this build) ro-arrays=uf (default)
// DEF-Z: result: sat
// PASS: check-inb.solver: yices (default) ro-arrays=array (default)
// CFG-Y: --seam
// CFG-Y: --solver=yices
// CFG-Y-NOT: --ro-arrays
// Z3: check-inb.solver: z3 (--solver) ro-arrays=uf (default)
// CFG-Z: --solver=z3
// CFG-Z: --ro-arrays=uf
// ROEXP: check-inb.solver: z3 (--solver) ro-arrays=array (--ro-arrays)
// CFG-R: --solver=z3
// CFG-R: --ro-arrays=array
// CFG-R-NOT: --ro-arrays=uf
// BZ: check-inb.solver: bitwuzla (--solver) ro-arrays=array (default)
// CFG-B: --solver=bitwuzla
// CFG-B-NOT: --ro-arrays
// LOGIC: check-inb.solver: z3 (--z3-logic) ro-arrays=uf (default)
// PARAM: check-inb.solver: z3 (--z3-param) ro-arrays=uf (default)
// NEUTRAL: check-inb.solver: yices (default) ro-arrays=array (default)
// IGN: Warning: the parts' verdict solver is yices, which ignores --z3-tactic [check-inb.z3-option-ignored]
// IGN-NOT: check-inb.z3-option-ignored
// IGN: check-inb.solver: yices (--solver) ro-arrays=array (default)
// JSON: "kind":"check-inb.solver"
// JSON-SAME: "solver":"z3"
// JSON-SAME: "ro_arrays":"uf"
// JSON-SAME: "ro_arrays_reason":"default"

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ag {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %c15 = sea.const 15 : i64
  %k = sea.sub %c15, %n : i64
  %q = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%q], %v inplace : !m, i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
