// cmp.congruence: a pointer to X is stored in slot i of a
// table of 8-byte slots and reloaded from slot j (byte memory, no
// widening). Byte k of the reload against byte l of the store compares
// T + 8i + l with T + 8j + k: for l != k the difference 8(i - j) + (l - k)
// is never 0 (8 divides the variable part, not l - k), exactly, for every
// i, j. cmp.congruence (default on) decides them in SeaM, leaving only
// 8i = 8j in each byte; with --seam-pull-concat the reload is then
// ite(8i = 8j, X, from_bits(older)). With --seam-congruence=false the
// rewriter's bit-vector rule bv.eq.low-zero2 decides the same compares once
// they are in linear form (8i + l - k = 8j); with neither
// (--seam-congruence=false --seam-simplify=0) they stay open.
// RUN: sea-vc %s --lower --seam --seam-stats --seam-congruence=false 2>&1 | FileCheck %s --check-prefixes=CHECK,OFF
// RUN: sea-vc %s --lower --seam --seam-stats --seam-simplify=0 --seam-congruence=false 2>&1 | FileCheck %s --check-prefixes=CHECK,RAW
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,ON
// RUN: sea-vc %s --lower --seam --seam-stats --seam-congruence --seam-pull-concat=4 2>&1 | FileCheck %s --check-prefixes=CHECK,PULL
// RUN: sea-vc %s --lower --seam --seam-congruence --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: not sea-vc %s --lower --seam-congruence 2>&1 | FileCheck %s --check-prefix=NEED
// The undecided ones are same-object alias questions (the census counts
// normal forms of the offset compares they reduce to): 1 (8i = 8j) when
// either rule decides the others, more with neither.
// OFF: seam.alias.open.all: 1
// OFF: seam.alias.open.same-object: 1
// OFF-NOT: cmp.congruence
// OFF: seam.rule.bv.eq.low-zero2: 56
// RAW-NOT: cmp.congruence
// RAW: seam.alias.open.all: {{[2-9]|[1-9][0-9]+}}
// ON: seam.alias.open.all: 1
// ON: seam.alias.open.same-object: 1
// ON: seam.rule.cmp.congruence: 56
// PULL: seam.rule.cmp.congruence: 56
// PULL: seam.rule.ptr.load-resolve-concat: 1
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// NEED: --seam-congruence{{.*}}need{{.*}}--seam
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @slots {
  %c4 = sea.const 4 : i64
  %c32 = sea.const 32 : i64
  %c8 = sea.const 8 : i64
  %T = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %X = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [3] : !m
  %i = sea.nondet "i" [4] : i64
  %j = sea.nondet "j" [5] : i64
  %iok = sea.icmp ult, %i, %c4 : i64
  sea.assume %iok
  %jok = sea.icmp ult, %j, %c4 : i64
  sea.assume %jok
  %pi = sea.ptr.index %T [%i] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %pj = sea.ptr.index %T [%j] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%pi], %X inplace : !m, !sea.ptr<64>
  %q = sea.load %m1[%pj] : !m -> !sea.ptr<64>
  %same = sea.icmp eq, %i, %j : i64
  %eq = sea.ptr.cmp eq, %q, %X : !sea.ptr<64>
  %t1 = sea.const 1 : i1
  %ns = sea.xor %same, %t1 : i1
  %ok = sea.or %ns, %eq : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
