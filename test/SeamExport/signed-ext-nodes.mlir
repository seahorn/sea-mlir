// sea-seam-export: the nodes slt, sle, sext and zext (signed compares
// return bool; an extension's width is its own `bits`).
// RUN: sea-seam-export %s --lane=raw | %python %S/Inputs/show.py | FileCheck %s
// CHECK: root %{{[0-9]+}} sea.sext bv: sext(nd.1_0, bits=16)
// CHECK: root %{{[0-9]+}} sea.zext bv: zext(nd.2_0, bits=16)
// CHECK: root %{{[0-9]+}} sea.icmp bv: ite(slt(sext(nd.1_0, bits=16), zext(nd.2_0, bits=16)), 1, 0)
// CHECK: root %{{[0-9]+}} sea.icmp bv: ite(sle(nd.1_0, nd.2_0), 1, 0)
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @ext {
  %x = sea.nondet "x" [1] : i8
  %y = sea.nondet "y" [2] : i8
  %s = sea.sext %x : i8 to i16
  %z = sea.zext %y : i8 to i16
  %lt = sea.icmp slt, %s, %z : i16
  %le = sea.icmp sle, %x, %y : i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
