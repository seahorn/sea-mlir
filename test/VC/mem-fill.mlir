// A global preloaded by sea.mem.fill (its initializer) and read back: the
// VC lowering expands the fill into stores (sea-expand-mem-fill) in every
// memory mode, with the same verdicts; a counterexample replays.
// RUN: sea-vc %s --lower --program=safe | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=bug | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program=bug --replay-original | FileCheck %s --check-prefix=ORIG
// RUN: sea-vc %s --lower --mem=lambda --program=safe | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --mem=lambda --program=bug | FileCheck %s --check-prefix=SAT
// Words: the memory is widened (the fill is 16 aligned bytes), and the fill
// becomes two 8-byte stores.
// RUN: sea-vc %s --lower --widen-memory=8 --program=safe | FileCheck %s --check-prefixes=UNSAT,WIDE
// RUN: sea-vc %s --lower --widen-memory=8 --program=bug | FileCheck %s --check-prefixes=SAT,WIDE
// RUN: sea-vc %s --lower --seam --program=safe | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --seam --program=bug | FileCheck %s --check-prefix=SAT
// Tracking: a tracked fill marks its object (sea-lower-tracking).
// RUN: sea-vc %s --lower --program=tracked | FileCheck %s --check-prefix=UNSAT
// The A-INB job checks the fill's range once: in bounds.
// RUN: sea-vc %s --lower --seam --ackermannize=off --check-inb --program=safe | FileCheck %s --check-prefix=INB
// RUN: sea-vc %s --lower --seam --ackermannize=off --check-inb --program=oob | FileCheck %s --check-prefix=INB-OOB
// Without --lower the VC refuses a fill.
// RUN: not sea-vc %s --program=placed 2>&1 | FileCheck %s --check-prefix=REFUSE
// RUN: sea-vc %s --lower --program=placed | FileCheck %s --check-prefix=UNSAT
// REQUIRES: z3

// WIDE: vc.widen-memory: {{.*}} widened-8=1
// UNSAT: result: unsat
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: validate: {{.*}} mismatches=0
// ORIG: replay.original: status=assert-failed {{.*}}match=yes
// INB: check-inb.parts: total=1 unsat=1 sat=0
// the first access, the fill, fails
// INB-OOB: check-inb.part: 0 id=0 {{.*}}result=sat
// REFUSE: sea.mem.fill is not encoded by the VC{{.*}}[vc.mem-fill]

!m = !sea.mem<!sea.ptr<64>, i8>
!a = !sea.mem<!sea.addr<64>, i8>

// one of the two words of the table, both >= 0x0807060504030201
sea.program @safe {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 16 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]> : tensor<16xi8> inplace : !m
  %i = sea.nondet "nd" [1] : i64
  %c1 = sea.const 1 : i64
  %j = sea.and %i, %c1 : i64
  %c8 = sea.const 8 : i64
  %off = sea.mul %j, %c8 : i64
  %q = sea.ptr.add %g, %off : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] align 8 : !m -> i64
  %lo = sea.const 578437695752307201 : i64
  %ok = sea.icmp uge, %x, %lo : i64
  sea.assert %ok
  sea.halt
}

// the first word is below the bound
sea.program @bug {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 16 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]> : tensor<16xi8> inplace : !m
  %i = sea.nondet "nd" [1] : i64
  %c1 = sea.const 1 : i64
  %j = sea.and %i, %c1 : i64
  %c8 = sea.const 8 : i64
  %off = sea.mul %j, %c8 : i64
  %q = sea.ptr.add %g, %off : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] align 8 : !m -> i64
  %lo = sea.const 578437695752307202 : i64
  %ok = sea.icmp uge, %x, %lo : i64
  sea.assert %ok
  sea.halt
}

sea.program @tracked {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %n = sea.const 4 : i64
  %a = sea.alloca %n align 4 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %n align 4 [2] : i64 -> !sea.ptr<64>
  sea.tracking_on
  %m1 = sea.mem.fill %m0[%a], dense<[1, 2]> : tensor<2xi8> inplace : !m
  sea.tracking_off
  %fa = sea.mem.is_modified %m1[%a] : !m
  %fb = sea.mem.is_modified %m1[%b] : !m
  %t = sea.const 1 : i1
  %nb = sea.xor %fb, %t : i1
  %ok = sea.and %fa, %nb : i1
  sea.assert %ok
  sea.halt
}

// 4 cells into an object of 3: the A-INB job finds it
sea.program @oob {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @small size 3 align 1 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4]> : tensor<4xi8> inplace : !m
  %x = sea.load %m1[%g] : !m -> i8
  %e = sea.const 1 : i8
  %ok = sea.icmp eq, %x, %e : i8
  sea.assert %ok
  sea.halt
}

// a placed program (BMC form, no lowering needed but the fill's)
sea.program @placed {
  %m0 = sea.mem.nondet "mem" [0] : !a
  %c16 = sea.const 16 : i64
  %p = sea.addr.from_int %c16 : i64 to !sea.addr<64>
  %m1 = sea.mem.fill %m0[%p], dense<[1, 2]> : tensor<2xi8> inplace : !a
  %x = sea.load %m1[%p] : !a -> i16
  %e = sea.const 513 : i16
  %ok = sea.icmp eq, %x, %e : i16
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
