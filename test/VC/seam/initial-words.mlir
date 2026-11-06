// Initial words (docs/design/SeaM-initial-words.md): an initial byte memory
// only read becomes an 8-byte word array. @eq reads an aligned word (proved
// from the allocation's alignment), a byte inside it, and an unaligned word
// and byte at a symbolic offset (one canonical masked part). @sat replays
// through the words model. @havoc reads a memhavoc array (same shape: also
// re-encoded). @trusted: an `align 8` on a load of a symbolic offset that
// the VC cannot prove (the fast path, counted). --words-check: the word
// query against the byte query.
// RUN: sea-vc %s --program=eq --lower --seam --seam-wide --seam-words=all --words-check --seam-stats 2>&1 | FileCheck %s --check-prefix=EQ
// RUN: sea-vc %s --program=eq --lower --seam --seam-wide --seam-words=all --words-check --solver=z3 2>&1 | FileCheck %s --check-prefix=RES-UNSAT
// RUN: sea-vc %s --program=eq --lower --seam --seam-wide --seam-words=all --ackermannize=off 2>&1 | FileCheck %s --check-prefix=RES-UNSAT
// RUN: sea-vc %s --program=offset --lower --seam --seam-wide --seam-words=all --words-check --seam-stats 2>&1 | FileCheck %s --check-prefix=OFFSET
// RUN: sea-vc %s --program=sat --lower --seam --seam-wide --seam-words=all --words-check 2>&1 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=sat --lower --seam --seam-wide --seam-words=all --ackermannize=off --solver=z3 2>&1 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=havoc --lower --seam --seam-wide --seam-words=all --words-check --seam-stats 2>&1 | FileCheck %s --check-prefix=HAVOC
// RUN: sea-vc %s --program=trusted --lower --seam --seam-wide --seam-words=all --words-check --seam-stats 2>&1 | FileCheck %s --check-prefix=TRUSTED
// RUN: not sea-vc %s --program=misleading --lower --seam --seam-wide --seam-words=all --words-check 2>&1 | FileCheck %s --check-prefix=MISLEAD
// RUN: sea-vc %s --program=misleading --lower --seam --seam-wide 2>&1 | FileCheck %s --check-prefix=RES-UNSAT
// RUN: sea-vc %s --program=eq --lower --seam --seam-wide --seam-words=auto --seam-stats 2>&1 | FileCheck %s --check-prefix=AUTO
// RUN: not sea-vc %s --program=eq --lower --seam-words=all 2>&1 | FileCheck %s --check-prefix=NOSEAM
// RUN: sea-vc %s --program=cross_array --lower --seam --seam-wide --seam-words=all --words-check 2>&1 | FileCheck %s --check-prefix=CROSS
// RUN: sea-vc %s --program=cross_array --lower --seam --seam-wide --seam-words=all --solver=z3 2>&1 | FileCheck %s --check-prefix=CROSS
// RUN: sea-vc %s --program=cross_array --lower --seam --seam-wide --seam-words=auto 2>&1 | FileCheck %s --check-prefix=CROSS
// RUN: sea-vc %s --program=cross_array --lower --seam --seam-wide --seam-words=all --ackermannize=off 2>&1 | FileCheck %s --check-prefix=CROSS
// EQ: words.check: ok
// EQ: words.chosen: 1
// EQ: words.word-reads: 1
// EQ: words.aligned-proved: 1
// EQ: words.masked-reads: 2
// EQ: words.masked-parts: 1
// EQ: words.array: name=nd.0_0 chosen=1
// EQ: vc.ackermann-reads: 3
// EQ: result: unsat
// RES-UNSAT: result: unsat
// OFFSET: words.check: ok
// OFFSET: words.masked-reads: 2
// OFFSET: result: unsat
// SAT: result: sat
// SAT: words.model: decoded={{[1-9][0-9]*}} defaulted=0
// SAT: replay: assert-failed
// SAT: validate: checked={{[0-9]+}} mismatches=0
// HAVOC: words.check: ok
// HAVOC: words.chosen: 2
// HAVOC: result: sat
// HAVOC: replay: assert-failed
// HAVOC: validate: checked={{[0-9]+}} mismatches=0
// TRUSTED: words.check: ok
// TRUSTED: words.aligned-by-annotation: 1
// TRUSTED: result: unsat
// MISLEAD: words.check: ok
// MISLEAD: result: sat
// MISLEAD: vc.words-align-trusted
// MISLEAD: vc.replay-no-failure
// AUTO: words.chosen: 0
// AUTO: words.array: name=nd.0_0 chosen=0 {{.*}} reason=byte-dominated
// NOSEAM: --seam-words needs --seam
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @eq {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %c3 = sea.const 3 : i64
  %c8 = sea.const 8 : i64
  %c1 = sea.const 1 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %iok = sea.icmp ult, %i, %c3 : i64
  sea.assume %iok
  %o = sea.mul %i, %c8 : i64
  %p = sea.ptr.add %a, %o : !sea.ptr<64>, i64
  %w = sea.load %m0[%p] align 8 : !m -> i64
  %p1 = sea.ptr.add %p, %c1 : !sea.ptr<64>, i64
  %b1 = sea.load %m0[%p1] : !m -> i8
  %k = sea.nondet "k" [3] : i64
  %c20 = sea.const 20 : i64
  %kok = sea.icmp ult, %k, %c20 : i64
  sea.assume %kok
  %q = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %u = sea.load %m0[%q] align 1 : !m -> i64
  %ub = sea.load %m0[%q] : !m -> i8
  %ut = sea.trunc %u : i64 to i8
  %s8 = sea.const 8 : i64
  %ws = sea.lshr %w, %s8 : i64
  %wt = sea.trunc %ws : i64 to i8
  %ok1 = sea.icmp eq, %wt, %b1 : i8
  %ok2 = sea.icmp eq, %ut, %ub : i8
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}
sea.program @sat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %c3 = sea.const 3 : i64
  %c8 = sea.const 8 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %iok = sea.icmp ult, %i, %c3 : i64
  sea.assume %iok
  %o = sea.mul %i, %c8 : i64
  %p = sea.ptr.add %a, %o : !sea.ptr<64>, i64
  %w = sea.load %m0[%p] align 8 : !m -> i64
  %k = sea.nondet "k" [3] : i64
  %c20 = sea.const 20 : i64
  %kok = sea.icmp ult, %k, %c20 : i64
  sea.assume %kok
  %q = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %u = sea.load %m0[%q] align 1 : !m -> i32
  %c77 = sea.const 77 : i32
  %ue = sea.icmp eq, %u, %c77 : i32
  %wz = sea.const 1234605616436508552 : i64
  %we = sea.icmp eq, %w, %wz : i64
  %both = sea.and %ue, %we : i1
  %t = sea.const 1 : i1
  %ok = sea.xor %both, %t : i1
  sea.assert %ok
  sea.halt
}
sea.program @havoc {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %c8 = sea.const 8 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %p8 = sea.ptr.add %a, %c8 : !sea.ptr<64>, i64
  %v1 = sea.const 1 : i8
  %m01 = sea.store %m0[%p8], %v1 inplace : !m, i8
  %m1 = sea.memhavoc %m01[%p8], %c8 [5] inplace : !m
  %w0 = sea.load %m1[%a] align 8 : !m -> i64
  %w1 = sea.load %m1[%p8] align 8 : !m -> i64
  %x = sea.xor %w0, %w1 : i64
  %c5 = sea.const 5 : i64
  %ok = sea.icmp ne, %x, %c5 : i64
  sea.assert %ok
  sea.halt
}
sea.program @trusted {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 64 : i64
  %c7 = sea.const 7 : i64
  %a = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %c56 = sea.const 56 : i64
  %iok = sea.icmp ult, %i, %c56 : i64
  sea.assume %iok
  %m8 = sea.const -8 : i64
  %i8 = sea.and %i, %m8 : i64
  %p = sea.ptr.add %a, %i8 : !sea.ptr<64>, i64
  %w = sea.load %m0[%p] align 8 : !m -> i64
  %b = sea.load %m0[%p] : !m -> i8
  %t = sea.trunc %w : i64 to i8
  %ok = sea.icmp eq, %t, %b : i8
  sea.assert %ok
  sea.halt
}
sea.program @misleading {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 64 : i64
  %c7 = sea.const 7 : i64
  %a = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %c56 = sea.const 56 : i64
  %iok = sea.icmp ult, %i, %c56 : i64
  sea.assume %iok
  %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
  %w = sea.load %m0[%p] align 8 : !m -> i64
  %b = sea.load %m0[%p] : !m -> i8
  %t = sea.trunc %w : i64 to i8
  %ok = sea.icmp eq, %t, %b : i8
  sea.assert %ok
  sea.halt
}
// @offset: every read of the symbolic part a + k has a nonzero numeral
// (k + 3, k + 4): the masked decoding places bytes at the numeral from S
// (a bug placed them at numeral - first numeral seen: caught by
// --words-check on fuzz-seam and vcc, not by the cases above, all at 0)
sea.program @offset {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %k = sea.nondet "k" [2] : i64
  %c20 = sea.const 20 : i64
  %kok = sea.icmp ult, %k, %c20 : i64
  sea.assume %kok
  %q = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %q3 = sea.ptr.add %q, %c3 : !sea.ptr<64>, i64
  %q4 = sea.ptr.add %q, %c4 : !sea.ptr<64>, i64
  %w = sea.load %m0[%q3] align 1 : !m -> i32
  %b = sea.load %m0[%q4] : !m -> i8
  %s8 = sea.const 8 : i32
  %ws = sea.lshr %w, %s8 : i32
  %wt = sea.trunc %ws : i32 to i8
  %ok = sea.icmp eq, %wt, %b : i8
  sea.assert %ok
  sea.halt
}

// @cross_array: two independent
// initial memories read at one address. Their words are two reads. The
// word cache was keyed by the index alone, so the second array reused the
// first array's word: a wrong unsat. Keyed by (array, index) now.
// CROSS-NOT: words.check: mismatch
// CROSS: result: sat
// CROSS: replay: assert-failed
// CROSS: validate: checked={{[0-9]+}} mismatches=0
!mc = !sea.mem<!sea.ptr<64>, i8>
sea.program @cross_array {
  %m0 = sea.mem.nondet "a" [0] : !mc
  %m1 = sea.mem.nondet "b" [1] : !mc
  %size = sea.const 8 : i64
  %p = sea.malloc %size align 8 [2] : i64 -> !sea.ptr<64>
  %a = sea.load %m0[%p] align 8 : !mc -> i64
  %b = sea.load %m1[%p] align 8 : !mc -> i64
  %equal = sea.icmp eq, %a, %b : i64
  sea.assert %equal
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
