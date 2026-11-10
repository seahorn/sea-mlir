// Flow events of an unsat run: the vc.solve event has the result, and no
// replay phase follows.
// RUN: sea-vc %s '--log=vc.phase,vc.solve' 2>&1 | FileCheck %s
// CHECK: log: vc.phase start phase=solve{{$}}
// CHECK-NEXT: log: vc.solve backend=z3 result=unsat time={{[0-9.]+}} nodes-made={{[0-9]+}}{{$}}
// CHECK-NEXT: log: vc.phase end phase=solve time=
// CHECK-NOT: phase=replay
// CHECK: result: unsat
// CHECK-NOT: phase=replay
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %ok = sea.icmp eq, %x, %x : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
