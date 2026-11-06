// RUN: sea-vc %s --lower | FileCheck %s

// Identities of the intrinsics hold for all inputs:
// umin(x, y) <= umax(x, y), bswap(bswap(x)) = x, fshl(x, x, s) = fshr(x, x,
// 16 - s mod 16) (rotations), and smul overflow implies the 32-bit product
// differs from the sign-extended 16-bit one.
// CHECK: result: unsat
sea.program @main {
  %x = sea.nondet "nondet_u16" [0] : i16
  %y = sea.nondet "nondet_u16" [1] : i16
  %s = sea.nondet "nondet_u16" [2] : i16
  %lo = sea.umin %x, %y : i16
  %hi = sea.umax %x, %y : i16
  %c0 = sea.icmp ule, %lo, %hi : i16
  %b = sea.bswap %x : i16
  %bb = sea.bswap %b : i16
  %c1 = sea.icmp eq, %bb, %x : i16
  %sixteen = sea.const 16 : i16
  %sm = sea.urem %s, %sixteen : i16
  %rs = sea.sub %sixteen, %sm : i16
  %l = sea.fshl %x, %x, %s : i16
  %r = sea.fshr %x, %x, %rs : i16
  %c2 = sea.icmp eq, %l, %r : i16
  %ov = sea.smulo %x, %y : i16
  %xw = sea.sext %x : i16 to i32
  %yw = sea.sext %y : i16 to i32
  %pw = sea.mul %xw, %yw : i32
  %p = sea.mul %x, %y : i16
  %pe = sea.sext %p : i16 to i32
  %differ = sea.icmp ne, %pw, %pe : i32
  %c3 = sea.icmp eq, %ov, %differ : i1
  %a01 = sea.and %c0, %c1 : i1
  %a23 = sea.and %c2, %c3 : i1
  %all = sea.and %a01, %a23 : i1
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
