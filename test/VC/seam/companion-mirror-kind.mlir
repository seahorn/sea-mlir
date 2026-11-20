// The mirror check of the companion exemption matches access kinds: a
// keyed store needs a data store at the same address in its block, a keyed
// load a data load; a keyed range op a data range op with the same operands
// in the same role (written: memset / memhavoc / memmove destination; read:
// memmove source). A-SEP exempts a read and a read, so a keyed store
// mirrored only by a data load is not backed by any data pair A-SEP covers.
//
// @kind: A = [4096, 4104), B = [4104, 4112), q = A + i with i = 8 (B's first
// cell). The data accesses are two loads, m[B] and m[q]: A-SEP holds for
// them. The companion side stores k[q] = 2, then k[B] = 1, and reads k[q]:
// the stores have no data store to mirror them, so they are program
// accesses (counted, warned), and the keyed load stays exempt (mirrored by
// the data load at q). Plain: sat; the run fails.
// @range: a data memmove A <- B reads B; a keyed memset writes B: not
// mirrored by the data source range.
// @keyedload: two unmirrored keyed loads at B - 4 (a gap cell) and a flagged
// data load at A + 12, the same cell: two reads, so the out-of-object
// report pairs nothing (the fallback keeps the keyed access's kind).
// RUN: sea-vc %s --program=kind | FileCheck %s --check-prefix=PLAIN
// RUN: sea-run %s --program=kind --missing=zero --input=3=8 | FileCheck %s --check-prefix=INTERP
// RUN: sea-vc %s --program=kind --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=KIND
// RUN: sea-vc %s --program=range --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=RANGE
// RUN: sea-vc %s --program=keyedload --seam 2>&1 | FileCheck %s --check-prefix=KLOAD
// PLAIN: result: sat
// INTERP: status: assert-failed
// KIND: Warning: a keyed (companion) access with no program access {{.*}}companion-mirror-kind.mlir":[[@LINE+24]]:{{.*}}[vc.seam-companion-unmirrored]
// KIND: seam.rule.companion.access: 3
// KIND: seam.rule.companion.unmirrored: 2
// RANGE: Warning: a keyed (companion) access with no program access {{.*}}companion-mirror-kind.mlir":[[@LINE+37]]:{{.*}}[vc.seam-companion-unmirrored]
// RANGE: seam.rule.companion.unmirrored: 1
// KLOAD: vc.seam-companion-unmirrored
// KLOAD-NOT: lands=shared
// KLOAD: seam.out-of-object: accesses=3 object=0 shared=0 unknown=0 null=0 gap=3
!m = !sea.mem<!sea.addr<64>, i8>
!k = !sea.mem<!sea.addr<64>, i64, keyed>
sea.program @kind {
  %A = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %B = sea.segment "heap.2" base 4104 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [1] : !m
  %k0 = sea.mem.nondet "k" [2] : !k
  %i = sea.nondet "i" [3] : i64
  %c8 = sea.const 8 : i64
  %ieq = sea.icmp eq, %i, %c8 : i64
  sea.assume %ieq
  %q = sea.addr.add %A, %i : !sea.addr<64>, i64
  %a = sea.const 1 : i64
  %b = sea.const 2 : i64
  %d0 = sea.load %m0[%B] : !m -> i8
  %d1 = sea.load %m0[%q] : !m -> i8
  %k1 = sea.store %k0[%q], %b inplace : !k, i64
  %k2 = sea.store %k1[%B], %a inplace : !k, i64
  %x = sea.load %k2[%q] : !k -> i64
  %ok = sea.icmp eq, %x, %b : i64
  sea.assert %ok
  sea.halt
}
sea.program @range {
  %A = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %B = sea.segment "heap.2" base 4104 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [1] : !m
  %k0 = sea.mem.nondet "k" [2] : !k
  %one = sea.const 1 : i64
  %z = sea.const 0 : i8
  %m1 = sea.memmove %m0[%A], %m0[%B], %one inplace : !m
  %d = sea.load %m1[%B] : !m -> i8
  %k1 = sea.memset %k0[%B], %z, %one inplace : !k
  %x = sea.load %k1[%B] : !k -> i64
  %zz = sea.const 0 : i64
  %ok = sea.icmp eq, %x, %zz : i64
  sea.assert %ok
  sea.halt
}
sea.program @keyedload {
  %A = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %B = sea.segment "heap.2" base 4112 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [1] : !m
  %k0 = sea.mem.nondet "k" [2] : !k
  %c12 = sea.const 12 : i64
  %cm4 = sea.const -4 : i64
  %p = sea.addr.add %A, %c12 : !sea.addr<64>, i64
  %q = sea.addr.add %B, %cm4 : !sea.addr<64>, i64
  %d = sea.load %m0[%p] : !m -> i8
  %x = sea.load %k0[%q] : !k -> i64
  %y = sea.load %k0[%q] : !k -> i64
  %ok = sea.icmp eq, %x, %y : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
