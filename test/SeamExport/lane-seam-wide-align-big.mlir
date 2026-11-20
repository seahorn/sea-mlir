// sea-seam-export, lane seam-wide: the ALIGN premise of an access whose
// `align` exceeds the address space (2^17 > 2^16). The mask `al - 1` is
// reduced to the address width: all ones (0xffff), so only address 0 is
// aligned; the numeral 0x1ffff does not fit 16 bits.
// RUN: sea-seam-export %s --lane=seam-wide --address-bits=16 --firings | %python %S/Inputs/show.py | FileCheck %s
// CHECK: firing 0 rules=wide.row-aligned premises=ALIGN op=load units=2
// CHECK: premise: and(true, eq(bvand(addr(index(obj.a, nd.3_0*1, c=0, K=0, plain)), 0xffff), 0), eq(bvand(addr(index(obj.a, nd.2_0*1, c=0, K=0, plain)), 0xffff), 0))
!am = !sea.mem<!sea.addr<16>, i8>
sea.program @wide {
  %a = sea.segment "a" base 16 : i16 size 8 : i16 align 8 kind "stack" : !sea.addr<16>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v = sea.nondet "v" [1] : i16
  %k = sea.nondet "k" [2] : i16
  %j = sea.nondet "j" [3] : i16
  %ak = sea.addr.add %a, %k : !sea.addr<16>, i16
  %aj = sea.addr.add %a, %j : !sea.addr<16>, i16
  %m1 = sea.store %m0[%ak], %v inplace align 131072 : !am, i16
  %x = sea.load %m1[%aj] align 131072 : !am -> i16
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
