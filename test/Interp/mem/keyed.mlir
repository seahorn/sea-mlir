// RUN: sea-run %s --program=keyed --missing=zero | FileCheck %s --check-prefix=INT
// RUN: sea-opt %s | sea-opt | FileCheck %s --check-prefix=RT
// RUN: sea-vc %s --program=keyed --lower | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --program=keyed --lower --seam | FileCheck %s --check-prefix=VC
// RUN: sea-opt %s --sea-place-static | sea-run - --program=keyed --missing=zero | FileCheck %s --check-prefix=INT
// RUN: sea-vc %s --program=keyed --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=MIRROR
// RUN: sea-vc %s --program=overwritten --lower | FileCheck %s --check-prefix=NEG
// RUN: sea-vc %s --program=overwritten --lower --seam | FileCheck %s --check-prefix=NEG

// The keyed layout: a cell of any width at every byte
// address, independent of its neighbours. A store at p + 1 does not touch
// the cell at p. memset repeats its byte across a cell; memset and
// memmove act on every address of their range. It is the layout of the
// fat-pointer companions (sea-lower-fat-pointers).
// RT: !sea.mem<!sea.ptr<64>, i64, keyed>
// INT: status: halted
// VC: result: unsat
// These keyed accesses are hand-written: no program access at their
// addresses mirrors them, so they are not exempt from A-INB (the
// companion exemption needs a mirrored data access in the same block).
// MIRROR: vc.seam-companion-unmirrored
// MIRROR: seam.inb: assumed accesses=8
// MIRROR: seam.rule.companion.unmirrored: 8
// @overwritten claims the store at p + 1 changed the cell at p: false, so
// the assertion fails (sat, replayed and validated): the VC does not read
// keyed cells as covering bytes.
// NEG: result: sat
// NEG: validate: checked={{[0-9]+}} mismatches=0
!k = !sea.mem<!sea.ptr<64>, i64, keyed>
sea.program @keyed {
  %m0 = sea.mem.nondet "m" [0] : !k
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %c4 = sea.const 4 : i64
  %c8 = sea.const 8 : i64
  %p1 = sea.ptr.add %p, %c1 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %p, %c4 : !sea.ptr<64>, i64
  %p5 = sea.ptr.add %p4, %c1 : !sea.ptr<64>, i64
  %p8 = sea.ptr.add %p, %c8 : !sea.ptr<64>, i64
  %p9 = sea.ptr.add %p8, %c1 : !sea.ptr<64>, i64
  %a = sea.const 0x1111111111111111 : i64
  %b = sea.const 0x2222222222222222 : i64
  %m1 = sea.store %m0[%p], %a inplace : !k, i64
  %m2 = sea.store %m1[%p1], %b inplace : !k, i64
  // independent cells: p still holds a
  %x = sea.load %m2[%p] : !k -> i64
  %ok1 = sea.icmp eq, %x, %a : i64
  // memset of 2 addresses at p + 4: each cell is the byte repeated
  %v = sea.const 0xab : i8
  %c2 = sea.const 2 : i64
  %m3 = sea.memset %m2[%p4], %v, %c2 inplace : !k
  %y = sea.load %m3[%p5] : !k -> i64
  %splat = sea.const 0xabababababababab : i64
  %ok2 = sea.icmp eq, %y, %splat : i64
  // memmove of 2 addresses from p to p + 8: both cells copied
  %m4 = sea.memmove %m3[%p8], %m3[%p], %c2 inplace : !k
  %z = sea.load %m4[%p9] : !k -> i64
  %ok3 = sea.icmp eq, %z, %b : i64
  %ok12 = sea.and %ok1, %ok2 : i1
  %ok = sea.and %ok12, %ok3 : i1
  sea.assert %ok
  sea.halt
}
sea.program @overwritten {
  %m0 = sea.mem.nondet "m" [0] : !k
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %p1 = sea.ptr.add %p, %c1 : !sea.ptr<64>, i64
  %a = sea.const 0x1111111111111111 : i64
  %b = sea.const 0x2222222222222222 : i64
  %m1 = sea.store %m0[%p], %a inplace : !k, i64
  %m2 = sea.store %m1[%p1], %b inplace : !k, i64
  %x = sea.load %m2[%p] : !k -> i64
  %ok = sea.icmp ne, %x, %a : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
