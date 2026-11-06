// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

// CHECK-LABEL: sea.program @all_ops
sea.program @all_ops {
  // CHECK: %[[X:.*]] = sea.nondet "nondet_u32" [0] : i32
  %x = sea.nondet "nondet_u32" [0] : i32
  // CHECK: %[[C:.*]] = sea.const 7 : i32
  %c = sea.const 7 : i32
  // CHECK: sea.add %[[X]], %[[C]] : i32
  %0 = sea.add %x, %c : i32
  // CHECK: sea.sub
  %1 = sea.sub %x, %c : i32
  // CHECK: sea.mul
  %2 = sea.mul %x, %c : i32
  // CHECK: sea.udiv
  %3 = sea.udiv %x, %c : i32
  // CHECK: sea.sdiv
  %4 = sea.sdiv %x, %c : i32
  // CHECK: sea.urem
  %5 = sea.urem %x, %c : i32
  // CHECK: sea.srem
  %6 = sea.srem %x, %c : i32
  // CHECK: sea.and
  %7 = sea.and %x, %c : i32
  // CHECK: sea.or
  %8 = sea.or %x, %c : i32
  // CHECK: sea.xor
  %9 = sea.xor %x, %c : i32
  // CHECK: sea.shl
  %10 = sea.shl %x, %c : i32
  // CHECK: sea.lshr
  %11 = sea.lshr %x, %c : i32
  // CHECK: sea.ashr
  %12 = sea.ashr %x, %c : i32
  // CHECK: %[[LT:.*]] = sea.icmp slt, %[[X]], %[[C]] : i32
  %lt = sea.icmp slt, %x, %c : i32
  // CHECK: sea.select %[[LT]], %[[X]], %[[C]] : i32
  %s = sea.select %lt, %x, %c : i32
  // CHECK: sea.zext %[[X]] : i32 to i64
  %z = sea.zext %x : i32 to i64
  // CHECK: sea.sext %[[X]] : i32 to i64
  %e = sea.sext %x : i32 to i64
  // CHECK: sea.trunc %[[X]] : i32 to i8
  %t = sea.trunc %x : i32 to i8
  // CHECK: sea.assume %[[LT]]
  sea.assume %lt
  // CHECK: sea.cond_br %[[LT]], ^bb1(%[[X]] : i32), ^bb2
  sea.cond_br %lt, ^bb1(%x : i32), ^bb2
^bb1(%a : i32):
  // CHECK: sea.br ^bb2
  sea.br ^bb2
^bb2:
  // CHECK: sea.assert %[[LT]]
  sea.assert %lt
  // CHECK: sea.halt
  sea.halt
}

// Wide and 1-bit bit-vectors are ordinary values.
// CHECK-LABEL: sea.program @widths
sea.program @widths {
  // Signless constants print signed; i1 prints as true/false.
  // CHECK: sea.const true
  %b = sea.const 1 : i1
  // CHECK: sea.const -1 : i128
  %w = sea.const 340282366920938463463374607431768211455 : i128
  %n = sea.xor %b, %b : i1
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
