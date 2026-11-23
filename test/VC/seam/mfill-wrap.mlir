// mfill on a range that wraps around the address space
// (docs/design/MemFill.md, "SeaM"; seam-lean rangeFill is circular): 10
// bytes at 250 of an 8-bit address space, bytes 6..9 at 0..3.
//  - @hit: s + 8 (address 2) is byte 8 = 9 (row.mfill-hit); s + 255
//    (address 249, just before the fill) is the old cell (row.mfill-skip).
//  - @wide: a 4-byte read at s + 4 inside the data, one at s + 8 across the
//    end of the data: with --seam-wide the first is one numeral
//    (wide.mfill-hit), the second byte by byte.
//  - @sym / @symbug: a symbolic offset i < 10 reads the expansion
//    (row.mfill-expand); the bug variant is sat and replays.
//  - @splatwrap: a wrapping fill of one byte is an mset
//    (mem.fill-replicate), read at any offset through one range test.
// Every firing is valid (--seam-check).
// RUN: sea-vc %s --lower --seam --seam-stats --program=hit 2>&1 | FileCheck %s --check-prefix=HIT
// RUN: sea-vc %s --lower --seam --seam-stats --seam-check --program=hit 2>&1 | FileCheck %s --check-prefixes=HIT,VALID
// RUN: sea-vc %s --lower --seam --seam-wide --seam-stats --seam-check --program=wide 2>&1 | FileCheck %s --check-prefixes=WIDE,VALID
// RUN: sea-vc %s --lower --seam --seam-stats --seam-check --program=sym 2>&1 | FileCheck %s --check-prefix=SYM
// RUN: sea-vc %s --lower --seam --seam-stats --program=symbug 2>&1 | FileCheck %s --check-prefixes=SYMBUG
// RUN: sea-vc %s --lower --seam --seam-stats --seam-check --program=splatwrap 2>&1 | FileCheck %s --check-prefixes=SPLAT,VALID
// REQUIRES: z3

// HIT-DAG: seam.rule.row.mfill-hit: 1
// HIT-DAG: seam.rule.row.mfill-skip: 1
// HIT: result: unsat
// WIDE-DAG: seam.rule.wide.mfill-hit: 1
// WIDE-DAG: seam.rule.row.mfill-hit: 2
// WIDE-DAG: seam.rule.row.mfill-skip: 2
// WIDE: result: unsat
// SYM-DAG: seam.rule.row.mfill-expand: 1
// SYM: result: unsat
// SYM: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0 unknown=0 unexpressible=0 covered=0 failed=0
// SYMBUG-DAG: seam.rule.row.mfill-expand: 1
// SYMBUG: replay: assert-failed
// SYMBUG: validate: {{.*}} mismatches=0
// SYMBUG: result: sat
// SPLAT-DAG: seam.rule.mem.fill-replicate: 1
// SPLAT-DAG: seam.rule.row.mset: 1
// SPLAT: result: unsat
// VALID: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0 unknown=0 unexpressible=0

!m = !sea.mem<!sea.addr<8>, i8>
// a fill of 10 bytes at 250 wraps: bytes 6..9 at 0..3
sea.program @hit {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c250 = sea.const 250 : i8
  %s = sea.addr.from_int %c250 : i8 to !sea.addr<8>
  %m1 = sea.mem.fill %m0[%s], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]> : tensor<10xi8> inplace : !m
  %c8 = sea.const 8 : i8
  %p = sea.addr.add %s, %c8 : !sea.addr<8>, i8
  %x = sea.load %m1[%p] : !m -> i8
  %c9 = sea.const 9 : i8
  %ok1 = sea.icmp eq, %x, %c9 : i8
  %cm1 = sea.const 255 : i8
  %q = sea.addr.add %s, %cm1 : !sea.addr<8>, i8
  %y = sea.load %m1[%q] : !m -> i8
  %y0 = sea.load %m0[%q] : !m -> i8
  %ok2 = sea.icmp eq, %y, %y0 : i8
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}
sea.program @wide {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c250 = sea.const 250 : i8
  %s = sea.addr.from_int %c250 : i8 to !sea.addr<8>
  %m1 = sea.mem.fill %m0[%s], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]> : tensor<10xi8> inplace : !m
  %c4 = sea.const 4 : i8
  %p = sea.addr.add %s, %c4 : !sea.addr<8>, i8
  %x = sea.load %m1[%p] : !m -> i32
  %e = sea.const 134678021 : i32
  %ok1 = sea.icmp eq, %x, %e : i32
  %c8 = sea.const 8 : i8
  %q = sea.addr.add %s, %c8 : !sea.addr<8>, i8
  %y = sea.load %m1[%q] : !m -> i32
  %c10 = sea.const 10 : i8
  %q2 = sea.addr.add %s, %c10 : !sea.addr<8>, i8
  %z = sea.load %m0[%q2] : !m -> i16
  %z32 = sea.zext %z : i16 to i32
  %c16 = sea.const 16 : i32
  %zs = sea.shl %z32, %c16 : i32
  %lo = sea.const 2569 : i32
  %ey = sea.or %zs, %lo : i32
  %ok2 = sea.icmp eq, %y, %ey : i32
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}
sea.program @sym {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c250 = sea.const 250 : i8
  %s = sea.addr.from_int %c250 : i8 to !sea.addr<8>
  %m1 = sea.mem.fill %m0[%s], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]> : tensor<10xi8> inplace : !m
  %i = sea.nondet "i" [1] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %i, %c10 : i8
  sea.assume %lt
  %p = sea.addr.add %s, %i : !sea.addr<8>, i8
  %x = sea.load %m1[%p] : !m -> i8
  %c1 = sea.const 1 : i8
  %ip1 = sea.add %i, %c1 : i8
  %ok = sea.icmp eq, %x, %ip1 : i8
  sea.assert %ok
  sea.halt
}
sea.program @symbug {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c250 = sea.const 250 : i8
  %s = sea.addr.from_int %c250 : i8 to !sea.addr<8>
  %m1 = sea.mem.fill %m0[%s], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]> : tensor<10xi8> inplace : !m
  %i = sea.nondet "i" [1] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %i, %c10 : i8
  sea.assume %lt
  %p = sea.addr.add %s, %i : !sea.addr<8>, i8
  %x = sea.load %m1[%p] : !m -> i8
  %c8 = sea.const 8 : i8
  %ok = sea.icmp ne, %x, %c8 : i8
  sea.assert %ok
  sea.halt
}
sea.program @splatwrap {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c250 = sea.const 250 : i8
  %s = sea.addr.from_int %c250 : i8 to !sea.addr<8>
  %m1 = sea.mem.fill %m0[%s], dense<7> : tensor<10xi8> inplace : !m
  %i = sea.nondet "i" [1] : i8
  %p = sea.addr.add %s, %i : !sea.addr<8>, i8
  %x = sea.load %m1[%p] : !m -> i8
  %x0 = sea.load %m0[%p] : !m -> i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %i, %c10 : i8
  %c7 = sea.const 7 : i8
  %v = sea.select %lt, %c7, %x0 : i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
