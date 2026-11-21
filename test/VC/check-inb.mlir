// sea-vc --check-inb: the A-INB discharge job (docs/design/CheckInb.md).
// A bounds check before every access, one part per check left, each part
// an ordinary safety query with --seam --ackermannize=off.
// RUN: sea-vc %s --program=inb --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=INB
// RUN: %if yices %{ sea-vc %s --program=inb --check-inb --lower --seam --ackermannize=off --solver=yices | FileCheck %s --check-prefix=INB %}
// RUN: %if bitwuzla %{ sea-vc %s --program=inb --check-inb --lower --seam --ackermannize=off --solver=bitwuzla | FileCheck %s --check-prefix=INB %}
// RUN: sea-vc %s --program=inb --check-inb --check-inb-static=false --lower --seam --ackermannize=off | FileCheck %s --check-prefix=INB
// RUN: sea-vc %s --program=oob --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=OOB
// RUN: %if yices %{ sea-vc %s --program=oob --check-inb --lower --seam --ackermannize=off --solver=yices | FileCheck %s --check-prefix=OOB %}
// RUN: %if bitwuzla %{ sea-vc %s --program=oob --check-inb --lower --seam --ackermannize=off --solver=bitwuzla | FileCheck %s --check-prefix=OOB %}
// RUN: sea-vc %s --program=ag --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=AG
// RUN: %if yices %{ sea-vc %s --program=ag --check-inb --lower --seam --ackermannize=off --solver=yices | FileCheck %s --check-prefix=AG %}
// RUN: sea-vc %s --program=ag_alone --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=ALONE
// RUN: sea-vc %s --program=diag --check-inb --lower --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=DIAG
// RUN: sea-opt %s --sea-check-inb --sea-split-asserts=program=ag | FileCheck %s --check-prefix=PARTS
// RUN: sea-vc %s --program=implied --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=FOLD
// RUN: sea-vc %s --program=mhnull --check-inb --lower --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=MHNULL
// RUN: sea-vc %s --program=mhnull --check-inb --lower --seam --ackermannize=off --memhavoc-null=error 2>&1 | FileCheck %s --check-prefix=MHERR
// RUN: sea-vc %s --program=mhnotnull --check-inb --lower --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=MHNN
// RUN: sea-vc %s --program=prior --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=PRIOR
// RUN: printf -- '--check-inb\n--lower\n--seam\n--ackermannize=off\n' > %t.cfg
// RUN: sea-vc %s --program=oob --config=%t.cfg | FileCheck %s --check-prefix=CFG
// RUN: not sea-vc %s --program=oob --check-inb --lower --seam --ackermannize=off --z3-tactic=no-such-tactic | FileCheck %s --check-prefix=PERR
// RUN: not sea-vc %s --program=inb --check-inb --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=CONFIG
// RUN: not sea-vc %s --program=inb --check-inb --lower --seam --ackermannize=off --ptr-companion=check 2>&1 | FileCheck %s --check-prefix=JOBS
// RUN: not sea-vc %s --program=inb --check-inb --lower --seam 2>&1 | FileCheck %s --check-prefix=CONFIG
// RUN: not sea-vc %s --program=inb --check-inb --lower --ackermannize=off 2>&1 | FileCheck %s --check-prefix=CONFIG

!m = !sea.mem<!sea.ptr<64>, i8>

// In bounds: every part unsat, the job discharges A-INB and the premises
// of --seam it implies.
// INB: check-inb: checks=3
// INB-NOT: result=sat
// INB: check-inb.parts: total={{[1-9]}} unsat={{[1-9]}} sat=0 unknown=0 other=0 skipped=0
// INB: check-inb.discharges: a-inb a-sep a-null a-nowrap under={{[a-z0-9.,-]+}}
// INB: result: unsat
sea.program @inb {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %c15 = sea.const 15 : i64
  %lt = sea.icmp ult, %i, %c15 : i64
  sea.assume %lt
  %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
  %v = sea.const 1 : i16
  %m1 = sea.store %m0[%p], %v inplace : !m, i16
  %x = sea.load %m1[%p] : !m -> i8
  %z = sea.const 0 : i8
  %m2 = sea.memset %m1[%a], %z, %c16 inplace : !m
  // the program's own assertion is not the job's
  %no = sea.icmp eq, %x, %z : i8
  sea.assert %no
  sea.halt
}

// Out of bounds: a + n with n free; the failing check is reported at the
// store's location, and the job discharges nothing.
// OOB: check-inb.part: 0 id=0 at={{.*}}check-inb.mlir:[[@LINE+11]]:{{[0-9]+}} result=sat status=0
// OOB-SAME: fail-at={{.*}}check-inb.mlir:[[@LINE+10]]
// OOB: check-inb.discharges: none
// OOB: result: sat
sea.program @oob {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  sea.halt
}

// Assume-guarantee. The first store, a + n, fails for n > 15. The second,
// a + (15 - n), is in bounds exactly when n <= 15 (else 15 - n wraps to a
// huge offset): only if the first check held. Its part assumes the first
// check, so it is unsat; the first part is sat.
// AG: check-inb.part: 0 id=0 {{.*}} result=sat
// AG: check-inb.part: 1 id=1 {{.*}} result=unsat
// AG: result: sat
// PARTS-LABEL: sea.program @ag.assert1
// PARTS: %[[D0:.*]] = sea.ptr.dereferenceable
// PARTS-NEXT: sea.assume %[[D0]] {sea.inb_check = 0 : i64}
// PARTS: sea.assert %{{.*}} {sea.inb_check = 1 : i64}
// PARTS-NEXT: sea.halt
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

// The control: the second store alone (no earlier check to assume) fails.
// ALONE: check-inb.part: 0 id=0 {{.*}} result=sat
// ALONE: result: sat
sea.program @ag_alone {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %v = sea.const 7 : i8
  %c15 = sea.const 15 : i64
  %k = sea.sub %c15, %n : i64
  %q = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %m2 = sea.store %m0[%q], %v inplace : !m, i8
  sea.halt
}

// The job lowers (and places) once: placement assumes the symbolic size
// fits, a warning of the job itself.
// DIAG: Warning: {{.*}}[place.symbolic-size-assume]
// The verdict rests on that assumption: it is on the discharges line.
// A-NOWRAP is decided by the program's objects (the symbolic one at its
// room).
// DIAG: check-inb.discharges: a-inb a-sep a-null a-nowrap under={{.*}}place.symbolic-size-assume,symbolic-size-bound
sea.program @diag {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.nondet "n" [1] : i64
  %c1 = sea.const 1 : i64
  %c8 = sea.const 8 : i64
  %ge = sea.icmp uge, %n, %c1 : i64
  sea.assume %ge
  %le = sea.icmp ule, %n, %c8 : i64
  sea.assume %le
  %a = sea.malloc %n align 8 [2] : i64 -> !sea.ptr<64>
  %x = sea.load %m0[%a] : !m -> i8
  sea.halt
}

// An input that is a part of an earlier split already, with no check left:
// no part, unsat.
// PRIOR: check-inb: checks=1 static=1 left=0
// PRIOR: check-inb.parts: total=0
// PRIOR: result: unsat
sea.program @prior attributes {sea.split_assert = {count = 2 : i64, index = 1 : i64}} {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %x = sea.load %m0[%a] : !m -> i8
  sea.halt
}

// --check-inb from a --config file: the parts do not run the job again.
// CFG: check-inb.part: 0 {{.*}} result=sat status=0
// CFG: result: sat

// A part that fails says why without --check-inb-dir.
// PERR: check-inb.part: 0 {{.*}} result=error status=1 {{.*}}error=vc.z3-tactic
// PERR: check-inb.part-error: 0 Error: {{.*}}[vc.z3-tactic]
// PERR: check-inb.part-diags: vc.z3-tactic=1
// PERR: result: unknown

// A check implied by an earlier one (the same span of the same pointer)
// folds before the split: one part, not two.
// FOLD: check-inb.split: folded=1 parts=1
// FOLD: result: sat
sea.program @implied {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %x = sea.load %m0[%p] : !m -> i8
  %y = sea.load %m0[%p] : !m -> i8
  sea.halt
}

// A memhavoc of the null pointer itself: a no-op by default, so its check
// passes (no size-0 warning) and folds away, and the result rests on that
// decision; with --memhavoc-null=error it fails.
// MHNULL-NOT: check-inb.size0-object
// MHNULL: check-inb.split: folded=1 parts=0
// MHNULL: check-inb.discharges: {{.*}} under=addr0-is-null,memhavoc-null-noop
// MHNULL: result: unsat
// MHERR: Warning: {{.*}}[check-inb.size0-object]
// MHERR: check-inb.part: 0 id=0 at={{.*}}check-inb.mlir:[[LOC:[0-9]+:[0-9]+]] {{.*}}fail-at={{.*}}check-inb.mlir:[[LOC]]{{$}}
// MHERR: result: sat
sea.program @mhnull {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %null = sea.ptr.null : !sea.ptr<64>
  %m2 = sea.memhavoc %m0[%null], %c16 [5] inplace : !m
  sea.halt
}

// A memhavoc of a pointer that cannot be null rests on no null decision.
// MHNN-NOT: memhavoc-null-noop
// MHNN: result: unsat
sea.program @mhnotnull {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %lt = sea.icmp ule, %n, %c16 : i64
  sea.assume %lt
  %m2 = sea.memhavoc %m0[%a], %n [5] inplace : !m
  %x = sea.load %m2[%a] : !m -> i8
  sea.halt
}

// --ptr-companion=check replaces the property of each part: refused.
// JOBS: Error: {{.*}}--ptr-companion=check{{.*}}[vc.check-jobs]
// CONFIG: --check-inb runs with --lower --seam --ackermannize=off{{.*}}[vc.check-inb-config]

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
