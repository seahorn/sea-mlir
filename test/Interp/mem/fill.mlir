// RUN: sea-run %s --program=placed --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=global --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=words --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-apply-tracking | sea-run - --program=tracked --missing=zero | FileCheck %s --check-prefix=OK
// RUN: not sea-run %s --program=out_of_bounds --missing=zero 2>&1 | FileCheck %s --check-prefix=OOB
// RUN: not sea-run %s --program=not_inplace --missing=zero 2>&1 | FileCheck %s --check-prefix=INPLACE

// sea.mem.fill writes its cells, in order, from the address; the cells
// around stay.

// OK: status: halted
// OOB: out of bounds: offset 3
// INPLACE: in place only

!m8 = !sea.mem<!sea.addr<8>, i8>
!m = !sea.mem<!sea.ptr<64>, i8>
!w = !sea.mem<!sea.addr<8>, i32, aligned>

// cells 10, 11, 12 = 1, 2, 3; 9 and 13 keep their (zero) contents
sea.program @placed {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c10 = sea.const 10 : i8
  %a = sea.addr.from_int %c10 : i8 to !sea.addr<8>
  %m1 = sea.mem.fill %m0[%a], dense<[1, 2, 3]> : tensor<3xi8> inplace : !m8
  %c9 = sea.const 9 : i8
  %p9 = sea.addr.from_int %c9 : i8 to !sea.addr<8>
  %x = sea.load %m1[%p9] : !m8 -> i40
  // bytes 9..13 = 0, 1, 2, 3, 0, little-endian
  %e = sea.const 50462976 : i40
  %ok = sea.icmp eq, %x, %e : i40
  sea.assert %ok
  sea.halt
}

// a global preloaded with its initializer, read back as an i32
sea.program @global {
  %m0 = sea.mem.nondet "m" [0] : !m
  %g = sea.global @tab size 6 align 4 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6]> : tensor<6xi8> inplace : !m
  %c2 = sea.const 2 : i64
  %q = sea.ptr.add %g, %c2 : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] : !m -> i32
  %e = sea.const 100992003 : i32
  %ok = sea.icmp eq, %x, %e : i32
  sea.assert %ok
  sea.halt
}

// word layout: cell k at addr + 4k
sea.program @words {
  %m0 = sea.mem.nondet "m" [0] : !w
  %c4 = sea.const 4 : i8
  %a = sea.addr.from_int %c4 : i8 to !sea.addr<8>
  %m1 = sea.mem.fill %m0[%a], dense<[7, 9]> : tensor<2xi32> inplace : !w
  %c8 = sea.const 8 : i8
  %b = sea.addr.from_int %c8 : i8 to !sea.addr<8>
  %x = sea.load %m1[%b] : !w -> i32
  %e = sea.const 9 : i32
  %ok = sea.icmp eq, %x, %e : i32
  sea.assert %ok
  sea.halt
}

// a tracked fill marks its object
sea.program @tracked {
  %m0 = sea.mem.nondet "m" [0] : !m
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

// 4 cells into an object of 3
sea.program @out_of_bounds {
  %m0 = sea.mem.nondet "m" [0] : !m
  %g = sea.global @small size 3 align 1 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4]> : tensor<4xi8> inplace : !m
  sea.halt
}

sea.program @not_inplace {
  %m0 = sea.mem.nondet "m" [0] : !m
  %g = sea.global @tab size 4 align 1 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1]> : tensor<1xi8> : !m
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
