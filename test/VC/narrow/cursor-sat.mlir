// --narrow-offsets (docs/design/Ackermann.md, "Narrowed offsets") on a
// parser cursor: o_k = o_{k-1} + zext(x_{k-1}) + 2, a byte read at p + o_k
// each step, p a 4096-byte object (k = 12). Every read is an access of p:
// its index is p + zext(low_12(o_k)), and two reads compare their 12-bit
// offsets. Counted (vc.narrow-*), on the ledger (assume.a-inb), and a sat
// model replays and validates, with and without --vc-defs.
// --ack-decider=false: the difference decider drops these pairs (their
// offsets are bounded); the test needs them constrained.
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --vc-defs=hidden 2>&1 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --solver=yices 2>&1 | FileCheck %s %}
// RUN: %if bitwuzla %{ sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --ackermannize=on --solver=bitwuzla 2>&1 | FileCheck %s %}
// RUN: sea-vc %s --lower --seam --narrow-offsets 2>&1 | FileCheck %s --check-prefix=DEC
// RUN: %if yices %{ sea-vc %s --lower --seam --narrow-offsets --solver=yices 2>&1 | FileCheck %s --check-prefix=DEC %}
// CHECK: vc.narrow: on
// CHECK: vc.narrow-reads: 6
// CHECK: vc.narrow-objects: 1
// CHECK: vc.narrow-k-max: 12
// CHECK: vc.narrow-pairs: 15
// CHECK: vc.narrow-mixed: 0
// CHECK: assume.a-inb: narrow-reads=6 narrow-pairs=15
// CHECK: result: sat
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
// The difference decider drops the 15 narrowed read pairs; same verdict.
// DEC: vc.ackermann-decider-pairs: 15
// DEC: result: sat
// DEC: validate: checked={{[0-9]+}} mismatches=0
// The query compares 12-bit offsets; no read index is a 64-bit sum. With
// --vc-defs=hidden the shared prefixes are 12-bit names.
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// SMT: (bvadd ((_ zero_extend 4) ack.nd.0_0.0) #x002)
// SMT-NOT: (_ zero_extend 56)
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --vc-defs=hidden --smt2 2>/dev/null | FileCheck %s --check-prefix=DEFS
// DEFS: (declare-fun def.0 () (_ BitVec 12))
// DEFS-NOT: (declare-fun def.{{[0-9]+}} () (_ BitVec 64))
// --seam-check states the A-INB of each narrowed read's access and checks
// that its index equals the narrowed one under it.
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHK: narrow-check: indices=6 checked=6 valid=6 invalid=0 unknown=0 unexpressible=0
// --narrow-check runs the same obligations alone (no rule trail), also
// without solving the query.
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --narrow-check --vcgen-only 2>&1 | FileCheck %s --check-prefix=CHK
// Off by default; without the premises it narrows nothing.
// RUN: sea-vc %s --lower --seam --ack-decider=false 2>&1 | FileCheck %s --check-prefix=OFF
// OFF-NOT: vc.narrow
// OFF-NOT: assume.a-inb
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --seam-premises=0 2>&1 | FileCheck %s --check-prefix=NOPREM
// NOPREM: vc.narrow: off-no-premises
// NOPREM-NOT: assume.a-inb
// RUN: not sea-vc %s --lower --narrow-offsets 2>&1 | FileCheck %s --check-prefix=NOSEAM
// NOSEAM: --narrow-offsets needs --seam
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @cursor {
  %m = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 4096 : i64
  %p = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %c2 = sea.const 2 : i64
  %o0 = sea.const 0 : i64
  %q0 = sea.ptr.add %p, %o0 : !sea.ptr<64>, i64
  %x0 = sea.load %m[%q0] : !m -> i8
  %z1 = sea.zext %x0 : i8 to i64
  %s1 = sea.add %o0, %z1 : i64
  %o1 = sea.add %s1, %c2 : i64
  %q1 = sea.ptr.add %p, %o1 : !sea.ptr<64>, i64
  %x1 = sea.load %m[%q1] : !m -> i8
  %z2 = sea.zext %x1 : i8 to i64
  %s2 = sea.add %o1, %z2 : i64
  %o2 = sea.add %s2, %c2 : i64
  %q2 = sea.ptr.add %p, %o2 : !sea.ptr<64>, i64
  %x2 = sea.load %m[%q2] : !m -> i8
  %z3 = sea.zext %x2 : i8 to i64
  %s3 = sea.add %o2, %z3 : i64
  %o3 = sea.add %s3, %c2 : i64
  %q3 = sea.ptr.add %p, %o3 : !sea.ptr<64>, i64
  %x3 = sea.load %m[%q3] : !m -> i8
  %z4 = sea.zext %x3 : i8 to i64
  %s4 = sea.add %o3, %z4 : i64
  %o4 = sea.add %s4, %c2 : i64
  %q4 = sea.ptr.add %p, %o4 : !sea.ptr<64>, i64
  %x4 = sea.load %m[%q4] : !m -> i8
  %z5 = sea.zext %x4 : i8 to i64
  %s5 = sea.add %o4, %z5 : i64
  %o5 = sea.add %s5, %c2 : i64
  %q5 = sea.ptr.add %p, %o5 : !sea.ptr<64>, i64
  %x5 = sea.load %m[%q5] : !m -> i8
  %c7 = sea.const 7 : i8
  %ok = sea.icmp ne, %x5, %c7 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
