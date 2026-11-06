// RUN: sea-vc %s --lower --seam --seam-check --ptr-companion=exact | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-check --ptr-companion=check | FileCheck %s

// The A-INB premise of a firing at a pointer companion cell
// (--ptr-companion, PtrSort S6) spans 1 unit: a companion is a cell per
// unit, whose mirror discharges 1 unit (OpSem settleKeyed). The companion
// store at E + i (i < 4, heap.2 of size 4) is the offset-0 byte of a
// pointer; a later companion read, rewritten in the context of an 8-byte
// word store, skips it by cmp.diff-base. Sized by the pointer's width (8
// units) the premise was statically out of bounds in a 4-unit object, and
// the firing unexpressible.
// CHECK-NOT: statically out of bounds
// CHECK: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.addr<64>, i8>
!w = !sea.mem<!sea.addr<64>, i64, aligned>
sea.program @span {
  %A = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %E = sea.segment "heap.2" base 4112 : i64 size 4 : i64 align 8 kind "heap" : !sea.addr<64>
  %X = sea.segment "heap.3" base 4128 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %C = sea.segment "heap.4" base 4144 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [6] : !m
  %w0 = sea.mem.nondet "w" [7] : !w
  %i = sea.nondet "i" [1] : i64
  %c4 = sea.const 4 : i64
  %iok = sea.icmp ult, %i, %c4 : i64
  sea.assume %iok
  %m1 = sea.store %m0[%A], %X inplace : !m, !sea.addr<64>
  %Ei = sea.addr.add %E, %i : !sea.addr<64>, i64
  %bits = sea.addr.to_bits %X : !sea.addr<64> to i64
  %b0 = sea.extract %bits[0] : i64 -> i8
  %m2 = sea.store %m1[%Ei], %b0 inplace : !m, i8
  %q = sea.load %m2[%A] : !m -> !sea.addr<64>
  %w1 = sea.store %w0[%C], %q inplace : !w, !sea.addr<64>
  %r = sea.load %w1[%C] : !w -> !sea.addr<64>
  %rb = sea.addr.to_bits %r : !sea.addr<64> to i64
  %eq = sea.icmp eq, %rb, %bits : i64
  sea.assert %eq
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
