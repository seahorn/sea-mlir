// The mfill term (docs/design/MemFill.md, "SeaM"): with --seam a
// sea.mem.fill of a byte memory stays a fill (vc.expand-mem-fill kept=1),
// and reads over it are decided by the rules that line up with seam-lean's
// theorems:
//  - @hit: reads at numeral offsets inside the data are the data bytes
//    (row.mfill-hit; a 4-byte load is split into byte reads);
//  - @outside: a read past the data at a numeral offset reads the memory
//    below (row.mfill-skip), a read of another object too
//    (row.mfill-skip-disjoint, A-SEP);
//  - @undecided / @undecided_bug: a read at a symbolic offset reads the
//    store expansion (row.mfill-expand), with the verdict of the plain
//    encoding; the counterexample replays;
//  - @splat: a fill of one repeated byte is an mset (mem.fill-replicate),
//    a symbolic read of it one range test (row.mset).
// No fill term reaches the solver; every firing is valid (--seam-check).
// RUN: sea-vc %s --lower --seam --seam-stats --program=hit 2>&1 | FileCheck %s --check-prefixes=KEPT,HIT
// RUN: sea-vc %s --lower --seam --seam-stats --program=outside 2>&1 | FileCheck %s --check-prefix=OUT
// RUN: sea-vc %s --lower --seam --seam-stats --program=undecided 2>&1 | FileCheck %s --check-prefix=UND
// RUN: sea-vc %s --lower --seam --program=undecided_bug 2>&1 | FileCheck %s --check-prefix=BUG
// RUN: sea-vc %s --lower --program=undecided_bug 2>&1 | FileCheck %s --check-prefix=BUG
// RUN: sea-vc %s --lower --seam --seam-stats --program=splat 2>&1 | FileCheck %s --check-prefix=SPLAT
// RUN: sea-vc %s --lower --seam --dag-stats --program=hit 2>&1 | FileCheck %s --check-prefix=DAG
// RUN: sea-vc %s --lower --seam --dag-stats --program=undecided 2>&1 | FileCheck %s --check-prefix=DAG
// RUN: sea-vc %s --lower --seam --seam-check --program=hit 2>&1 | FileCheck %s --check-prefix=VALID
// RUN: sea-vc %s --lower --seam --seam-check --program=outside 2>&1 | FileCheck %s --check-prefix=VALID
// RUN: sea-vc %s --lower --seam --seam-check --program=undecided 2>&1 | FileCheck %s --check-prefix=VALID
// RUN: sea-vc %s --lower --seam --seam-check --program=splat 2>&1 | FileCheck %s --check-prefix=VALID
// With the rewriting off the boundary translates the reads (by the read
// equation of the term), same verdicts.
// RUN: sea-vc %s --lower --seam --seam-stats --seam-row-expand=0 --program=undecided 2>&1 | FileCheck %s --check-prefix=ROW0
// RUN: sea-vc %s --lower --seam --seam-row-expand=0 --program=undecided_bug 2>&1 | FileCheck %s --check-prefix=BUG
// REQUIRES: z3

// KEPT: vc.expand-mem-fill: expanded=0 cells=0 kept=1
// HIT-DAG: seam.rule.row.mfill-hit: 5
// HIT-DAG: seam.rule.memop.translated: 0
// HIT: result: unsat
// OUT-DAG: seam.rule.row.mfill-skip: 1
// OUT-DAG: seam.rule.row.mfill-skip-disjoint: 1
// OUT-DAG: seam.rule.premise.a-sep:
// OUT: result: unsat
// UND-DAG: seam.rule.row.mfill-expand: 1
// UND: result: unsat
// BUG: result: sat
// BUG: replay: assert-failed
// BUG: validate: {{.*}} mismatches=0
// SPLAT-DAG: seam.rule.mem.fill-replicate: 1
// SPLAT-DAG: seam.rule.row.mset: 1
// SPLAT-NOT: row.mfill-expand
// SPLAT: result: unsat
// DAG-NOT: vc.dag-op: mfill
// DAG: result: unsat
// VALID: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0 unknown=0 unexpressible=0
// ROW0: seam.rule.memop.translated: {{[1-9]}}
// ROW0: result: unsat

!m = !sea.mem<!sea.ptr<64>, i8>

// g[3] = 4 and the word at g + 4 is 0x08070605
sea.program @hit {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 16 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]> : tensor<16xi8> inplace : !m
  %c3 = sea.const 3 : i64
  %p3 = sea.ptr.add %g, %c3 : !sea.ptr<64>, i64
  %x = sea.load %m1[%p3] : !m -> i8
  %c4b = sea.const 4 : i8
  %ok1 = sea.icmp eq, %x, %c4b : i8
  %c4 = sea.const 4 : i64
  %p4 = sea.ptr.add %g, %c4 : !sea.ptr<64>, i64
  %w = sea.load %m1[%p4] align 4 : !m -> i32
  %wv = sea.const 134678021 : i32
  %ok2 = sea.icmp eq, %w, %wv : i32
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}

// g + 20 is past the 16 bytes of the fill, h is another object: both read
// the memory before the fill
sea.program @outside {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 32 align 8 : !sea.ptr<64>
  %h = sea.global @other size 8 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]> : tensor<16xi8> inplace : !m
  %c20 = sea.const 20 : i64
  %q = sea.ptr.add %g, %c20 : !sea.ptr<64>, i64
  %a1 = sea.load %m1[%q] : !m -> i8
  %a0 = sea.load %m0[%q] : !m -> i8
  %ok1 = sea.icmp eq, %a1, %a0 : i8
  %i = sea.nondet "i" [1] : i64
  %c8 = sea.const 8 : i64
  %lt = sea.icmp ult, %i, %c8 : i64
  sea.assume %lt
  %r = sea.ptr.add %h, %i : !sea.ptr<64>, i64
  %b1 = sea.load %m1[%r] : !m -> i8
  %b0 = sea.load %m0[%r] : !m -> i8
  %ok2 = sea.icmp eq, %b1, %b0 : i8
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}

// g[i] for a symbolic i < 16: every byte is at least 1
sea.program @undecided {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 16 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]> : tensor<16xi8> inplace : !m
  %i = sea.nondet "i" [1] : i64
  %c16 = sea.const 16 : i64
  %lt = sea.icmp ult, %i, %c16 : i64
  sea.assume %lt
  %p = sea.ptr.add %g, %i : !sea.ptr<64>, i64
  %x = sea.load %m1[%p] : !m -> i8
  %c1 = sea.const 1 : i8
  %ok = sea.icmp uge, %x, %c1 : i8
  sea.assert %ok
  sea.halt
}

// g[i] is 7 for i = 6
sea.program @undecided_bug {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 16 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]> : tensor<16xi8> inplace : !m
  %i = sea.nondet "i" [1] : i64
  %c16 = sea.const 16 : i64
  %lt = sea.icmp ult, %i, %c16 : i64
  sea.assume %lt
  %p = sea.ptr.add %g, %i : !sea.ptr<64>, i64
  %x = sea.load %m1[%p] : !m -> i8
  %c7 = sea.const 7 : i8
  %ok = sea.icmp ne, %x, %c7 : i8
  sea.assert %ok
  sea.halt
}

// a zero initializer read at a symbolic offset
sea.program @splat {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @zero size 64 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<0> : tensor<64xi8> inplace : !m
  %i = sea.nondet "i" [1] : i64
  %c64 = sea.const 64 : i64
  %lt = sea.icmp ult, %i, %c64 : i64
  sea.assume %lt
  %p = sea.ptr.add %g, %i : !sea.ptr<64>, i64
  %x = sea.load %m1[%p] : !m -> i8
  %c0 = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %c0 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
