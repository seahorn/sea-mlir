// RUN: sea-run %s --program=memset --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=move_forward --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=move_backward --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=move_regions --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=wrap --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=havoc --input=0@2=5,1@2=9,1@3=8 | FileCheck %s --check-prefix=HAVOC
// RUN: sea-run %s --program=havoc --missing=random --seed=3 | FileCheck %s --check-prefix=OK

!m8 = !sea.mem<!sea.addr<8>, i8>

// OK: status: halted

// memset of 3 cells from 10 leaves 13 alone.
sea.program @memset {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c10 = sea.const 10 : i8
  %a = sea.addr.from_int %c10 : i8 to !sea.addr<8>
  %v = sea.const 1 : i8
  %n = sea.const 3 : i8
  %m1 = sea.memset %m0[%a], %v, %n inplace : !m8
  %c12 = sea.const 12 : i8
  %c13 = sea.const 13 : i8
  %p12 = sea.addr.from_int %c12 : i8 to !sea.addr<8>
  %p13 = sea.addr.from_int %c13 : i8 to !sea.addr<8>
  %x = sea.load %m1[%p12] : !m8 -> i8
  %y = sea.load %m1[%p13] : !m8 -> i8
  %z = sea.const 0 : i8
  %a1 = sea.icmp eq, %x, %v : i8
  %a2 = sea.icmp eq, %y, %z : i8
  %ok = sea.and %a1, %a2 : i1
  sea.assert %ok
  sea.halt
}

// Overlapping moves read the source as it was before: [0..3] = 1,2,3,4.
// Forward: move [0..3) to [1..4) gives 1,1,2,3.
sea.program @move_forward {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %i0 = sea.const 0 : i8
  %i1 = sea.const 1 : i8
  %i2 = sea.const 2 : i8
  %i3 = sea.const 3 : i8
  %i4 = sea.const 4 : i8
  %p0 = sea.addr.from_int %i0 : i8 to !sea.addr<8>
  %p1 = sea.addr.from_int %i1 : i8 to !sea.addr<8>
  %p2 = sea.addr.from_int %i2 : i8 to !sea.addr<8>
  %p3 = sea.addr.from_int %i3 : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%p0], %i1 inplace : !m8, i8
  %m2 = sea.store %m1[%p1], %i2 inplace : !m8, i8
  %m3 = sea.store %m2[%p2], %i3 inplace : !m8, i8
  %m4 = sea.store %m3[%p3], %i4 inplace : !m8, i8
  %m5 = sea.memmove %m4[%p1], %m4[%p0], %i3 inplace : !m8
  %x = sea.load %m5[%p3] : !m8 -> i8
  %ok = sea.icmp eq, %x, %i3 : i8
  sea.assert %ok
  sea.halt
}

// Backward: move [1..4) to [0..3) gives 2,3,4,4.
sea.program @move_backward {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %i0 = sea.const 0 : i8
  %i1 = sea.const 1 : i8
  %i2 = sea.const 2 : i8
  %i3 = sea.const 3 : i8
  %i4 = sea.const 4 : i8
  %p0 = sea.addr.from_int %i0 : i8 to !sea.addr<8>
  %p1 = sea.addr.from_int %i1 : i8 to !sea.addr<8>
  %p2 = sea.addr.from_int %i2 : i8 to !sea.addr<8>
  %p3 = sea.addr.from_int %i3 : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%p0], %i1 inplace : !m8, i8
  %m2 = sea.store %m1[%p1], %i2 inplace : !m8, i8
  %m3 = sea.store %m2[%p2], %i3 inplace : !m8, i8
  %m4 = sea.store %m3[%p3], %i4 inplace : !m8, i8
  %m5 = sea.memmove %m4[%p0], %m4[%p1], %i3 inplace : !m8
  %x = sea.load %m5[%p2] : !m8 -> i8
  %y = sea.load %m5[%p3] : !m8 -> i8
  %a1 = sea.icmp eq, %x, %i4 : i8
  %a2 = sea.icmp eq, %y, %i4 : i8
  %ok = sea.and %a1, %a2 : i1
  sea.assert %ok
  sea.halt
}

// Between regions: the source chain is unchanged.
sea.program @move_regions {
  %s0 = sea.mem.nondet "src" [0] : !m8
  %d0 = sea.mem.nondet "dst" [1] : !m8
  %i5 = sea.const 5 : i8
  %i9 = sea.const 9 : i8
  %n = sea.const 1 : i8
  %p = sea.addr.from_int %i5 : i8 to !sea.addr<8>
  %s1 = sea.store %s0[%p], %i9 inplace : !m8, i8
  %d1 = sea.memmove %d0[%p], %s1[%p], %n inplace : !m8
  %x = sea.load %d1[%p] : !m8 -> i8
  %y = sea.load %s1[%p] : !m8 -> i8
  %a1 = sea.icmp eq, %x, %i9 : i8
  %a2 = sea.icmp eq, %y, %i9 : i8
  %ok = sea.and %a1, %a2 : i1
  sea.assert %ok
  sea.halt
}

// Ranges wrap at 2^W: memset of 2 cells from 255 covers 255 and 0.
sea.program @wrap {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %top = sea.const 255 : i8
  %a = sea.addr.from_int %top : i8 to !sea.addr<8>
  %v = sea.const 6 : i8
  %n = sea.const 2 : i8
  %m1 = sea.memset %m0[%a], %v, %n inplace : !m8
  %z = sea.const 0 : i8
  %p0 = sea.addr.from_int %z : i8 to !sea.addr<8>
  %x = sea.load %m1[%p0] : !m8 -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// memhavoc replaces only its range, and a read of a havocked cell is the
// same every time. Cell 2: written 5 before the havoc, havocked (site 1).
// HAVOC: memread: site=1 addr=2 width=8 value=9 origin=input
// HAVOC: status: halted
sea.program @havoc {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %i2 = sea.const 2 : i8
  %i4 = sea.const 4 : i8
  %one = sea.const 1 : i8
  %p2 = sea.addr.from_int %i2 : i8 to !sea.addr<8>
  %p4 = sea.addr.from_int %i4 : i8 to !sea.addr<8>
  %v = sea.const 5 : i8
  %m1 = sea.store %m0[%p2], %v inplace : !m8, i8
  %m2 = sea.store %m1[%p4], %v inplace : !m8, i8
  %n = sea.const 2 : i8
  %m3 = sea.memhavoc %m2[%p2], %n [1] inplace : !m8
  %x = sea.load %m3[%p2] : !m8 -> i8
  %x2 = sea.load %m3[%p2] : !m8 -> i8
  %y = sea.load %m3[%p4] : !m8 -> i8
  %a1 = sea.icmp eq, %x, %x2 : i8
  %a2 = sea.icmp eq, %y, %v : i8
  %ok = sea.and %a1, %a2 : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
