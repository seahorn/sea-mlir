// RUN: sea-vc %s --vc-canonical-sums --smt2 2>/dev/null | tr '\n' ' ' | tr -s ' ' | FileCheck %s

// Item 2 of the form (docs/design/CanonicalSums.md): one leaf order at
// every width, by base: x, zext x and a low extract of x share the base x.
// The 8-bit sum y + x and the 32-bit sum zext y + zext x both put x (nd.0)
// first; the 16-bit sum puts the low byte of v (nd.2, created last) last.
// CHECK-DAG: (bvadd nd.0_0 nd.1_0)
// CHECK-DAG: (bvadd ((_ zero_extend 24) nd.0_0) ((_ zero_extend 24) nd.1_0))
// CHECK-DAG: (bvadd ((_ zero_extend 8) nd.1_0) ((_ zero_extend 8) ((_ extract 7 0) nd.2_0)))
// Equal forms are one node: (x - y) + 3 and 3 + (x - y) are one term.
// CHECK-DAG: (bvule [[E:\(bvadd nd.0_0 \(bvnot nd.1_0\) #x04\)]] [[E]])
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %y = sea.nondet "f" [1] : i8
  %v = sea.nondet "f" [2] : i32
  %c3 = sea.const 3 : i8
  %c9 = sea.const 9 : i8
  %s8 = sea.add %y, %x : i8
  %u8 = sea.icmp ult, %s8, %c9 : i8
  sea.assume %u8
  %zx = sea.zext %x : i8 to i32
  %zy = sea.zext %y : i8 to i32
  %s32 = sea.add %zy, %zx : i32
  %c9w = sea.const 9 : i32
  %u32 = sea.icmp ult, %s32, %c9w : i32
  sea.assume %u32
  %lv = sea.trunc %v : i32 to i8
  %lvz = sea.zext %lv : i8 to i16
  %yz = sea.zext %y : i8 to i16
  %s16 = sea.add %lvz, %yz : i16
  %c9h = sea.const 9 : i16
  %u16 = sea.icmp ult, %s16, %c9h : i16
  sea.assume %u16
  %d = sea.sub %x, %y : i8
  %e1 = sea.add %d, %c3 : i8
  %e2 = sea.add %c3, %d : i8
  %ok = sea.icmp ule, %e1, %e2 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
