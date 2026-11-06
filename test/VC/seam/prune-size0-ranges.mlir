// addr.prune-size0 over memory-operation ranges (docs/design/SeaM-provenance.md):
// pruning a size-0 leaf of a range is an
// operation-level equivalence under the A-INB of the original range, checked
// by --seam-check as the equality of the two memories at a fresh cell, never
// as an address equality (A-INB admits n = 0 with the null guard true, where
// the addresses differ and both operations are no-ops).
// @dst: memmove into ite(c, null, A), symbolic n <= 1, read of B: the null
// leaf goes (addr.prune-size0-range), the read skips the copy. Unsat.
// @src: memmove from ite(c, null, B) into A: the source leaf goes; the read
// of A[0] equals B[0] when n = 1. Unsat under A-INB (plain is sat: n = 1
// with c true copies from address 0; the bounds job's violation).
// @allnull: memset through ite(c, null, Z), Z = malloc(0), symbolic n: every
// leaf is size 0, so A-INB gives n = 0 and the memset is a no-op
// (addr.prune-size0-noop), not a static violation (no fallback). Unsat.
// RUN: sea-vc %s --lower --program=dst --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=DST
// RUN: sea-vc %s --lower --program=dst --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: %if yices %{ sea-vc %s --lower --program=dst --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: sea-vc %s --lower --program=src --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=SRC
// RUN: sea-vc %s --lower --program=src --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: %if yices %{ sea-vc %s --lower --program=src --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: sea-vc %s --lower --program=allnull --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=ALL
// RUN: sea-vc %s --lower --program=allnull --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: %if yices %{ sea-vc %s --lower --program=allnull --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// DST-NOT: vc.seam-inb-violated
// DST: seam.inb: assumed
// DST-DAG: seam.rule.addr.prune-size0-range: {{[1-9]}}
// DST-DAG: seam.rule.row.mcopy-skip: {{[1-9]}}
// DST: result: unsat
// SRC-NOT: vc.seam-inb-violated
// SRC: seam.inb: assumed
// SRC-DAG: seam.rule.addr.prune-size0-range: {{[1-9]}}
// SRC: result: unsat
// ALL-NOT: vc.seam-inb-violated
// ALL: seam.inb: assumed
// ALL-DAG: seam.rule.addr.prune-size0-noop: {{[1-9]}}
// ALL: result: unsat
// CHK-NOT: vc.seam-inb-violated
// CHK: result: unsat
// CHK: seam-check: firings={{[1-9][0-9]*}} {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @dst {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c16 align 8 [5] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %n = sea.nondet "n" [4] : i64
  %one = sea.const 1 : i64
  %nok = sea.icmp ule, %n, %one : i64
  sea.assume %nok
  %null = sea.ptr.null : !sea.ptr<64>
  %p = sea.select %c, %null, %A : !sea.ptr<64>
  %y = sea.load %m0[%B] : !m -> i8
  %m1 = sea.memmove %m0[%p], %m0[%C], %n inplace : !m
  %x = sea.load %m1[%B] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @src {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %n = sea.nondet "n" [4] : i64
  %one = sea.const 1 : i64
  %zero = sea.const 0 : i64
  %nok = sea.icmp ule, %n, %one : i64
  sea.assume %nok
  %null = sea.ptr.null : !sea.ptr<64>
  %q = sea.select %c, %null, %B : !sea.ptr<64>
  %y = sea.load %m0[%B] : !m -> i8
  %m1 = sea.memmove %m0[%A], %m0[%q], %n inplace : !m
  %x = sea.load %m1[%A] : !m -> i8
  %eq = sea.icmp eq, %x, %y : i8
  %n0 = sea.icmp eq, %n, %zero : i64
  %ok = sea.or %n0, %eq : i1
  sea.assert %ok
  sea.halt
}
sea.program @allnull {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %c0 = sea.const 0 : i64
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %Z = sea.malloc %c0 align 8 [1] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %n = sea.nondet "n" [4] : i64
  %null = sea.ptr.null : !sea.ptr<64>
  %p = sea.select %c, %null, %Z : !sea.ptr<64>
  %v7 = sea.const 7 : i8
  %y = sea.load %m0[%B] : !m -> i8
  %m1 = sea.memset %m0[%p], %v7, %n inplace : !m
  %x = sea.load %m1[%B] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
