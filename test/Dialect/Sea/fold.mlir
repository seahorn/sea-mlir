// RUN: sea-opt %s --sccp | FileCheck %s
// RUN: sea-opt %s --canonicalize | FileCheck %s --check-prefix=CANON

// The pure ops fold through evalPureOp: 5 * 4 = 20; a constant select picks
// its arm. Ops with nsw / nuw are not folded (their promise would be lost),
// and program inputs (nondet sites) stay even when unused.

// CHECK-DAG: sea.const 20 : i8
// CHECK-DAG: sea.add %{{.*}}, %{{.*}} nsw : i8
// CHECK-DAG: sea.nondet "nd" [1] : i8
// CHECK-DAG: sea.nondet "unused" [3] : i8
// CHECK-DAG: sea.mem.nondet "r" [0]
// CHECK-DAG: sea.memhavoc
// CHECK-DAG: sea.struct.nondet "s" [4]
// CHECK-NOT: sea.mul
// CHECK-NOT: sea.select
// CANON-DAG: sea.nondet "unused" [3] : i8
// CANON-DAG: sea.mem.nondet "r" [0]
// CANON-DAG: sea.memhavoc
// CANON-DAG: sea.struct.nondet "s" [4]
// CANON-NOT: sea.mul
sea.program @main {
  %m0 = sea.mem.nondet "r" [0] : !sea.mem<!sea.addr<8>, i8>
  %x = sea.nondet "nd" [1] : i8
  %y = sea.nondet "unused" [3] : i8
  %st = sea.struct.nondet "s" [4] : !sea.struct<i8, i8>
  %a = sea.const 5 : i8
  %b = sea.const 4 : i8
  %n = sea.mul %a, %b : i8
  %f = sea.add %a, %b nsw : i8
  %t = sea.const 1 : i1
  %s = sea.select %t, %n, %x : i8
  %c = sea.icmp ult, %x, %s : i8
  %pa = sea.addr.from_int %n : i8 to !sea.addr<8>
  %m1 = sea.memhavoc %m0[%pa], %b [2] inplace : !sea.mem<!sea.addr<8>, i8>
  %u = sea.icmp ult, %x, %f : i8
  %ok = sea.and %c, %u : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
