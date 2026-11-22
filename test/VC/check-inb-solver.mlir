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
// RUN: %if yices %{ FileCheck %s --check-prefix=MAN-Y < %t.d/manifest.json %}
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
// MAN-Y: "--config=parts.cfg"
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
// JSON: "kind":"check-inb.solver"
// JSON-SAME: "solver":"z3"

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
