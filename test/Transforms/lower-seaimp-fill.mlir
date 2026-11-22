// RUN: sea-opt %s --sea-lower-seaimp | FileCheck %s
// RUN: sea-opt %s --sea-lower-seaimp | sea-run - --program=table --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-lower-seaimp | sea-run - --program=const_start --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-vc %s --lower --program=table | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=const_start | FileCheck %s --check-prefix=UNSAT
// REQUIRES: z3

// sea-lower-seaimp: seaimp.mem.fill is get / sea.mem.fill inplace / set;
// a memory variable with init starts as sea.mem.const of it (shadow kept),
// right after the variable (the init value is defined before it).

// OK: status: halted
// UNSAT: result: unsat

!m = !sea.mem<!sea.ptr<64>, i8>

// CHECK-LABEL: sea.program @table
// CHECK: %[[M0:.*]] = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
// CHECK: %[[G:.*]] = sea.global @tab size 4 align 4 : !sea.ptr<64>
// CHECK: %[[M1:.*]] = sea.mem.fill %[[M0]][%[[G]]], dense<[1, 2, 3, 4]> : tensor<4xi8> inplace tracked : !sea.mem<!sea.ptr<64>, i8>
// CHECK: sea.load %[[M1]][%[[G]]]
// CHECK-NOT: seaimp.
sea.program @table {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %g = seaimp.global %mem @tab size 4 align 4 : !seaimp.var<!m> -> !sea.ptr<64>
  seaimp.mem.fill %mem[%g], dense<[1, 2, 3, 4]> : tensor<4xi8> tracked : !seaimp.var<!m>
  %x = seaimp.load %mem[%g] : !seaimp.var<!m> -> i32
  %e = sea.const 67305985 : i32
  %ok = sea.icmp eq, %x, %e : i32
  sea.assert %ok
  sea.halt
}

// CHECK-LABEL: sea.program @const_start
// CHECK: %[[Z:.*]] = sea.const 9 : i8
// CHECK-NEXT: %[[S:.*]] = sea.mem.const %[[Z]] shadow : !sea.mem<!sea.ptr<64>, i8>
// CHECK: sea.mem.fill %[[S]]
// CHECK-NOT: seaimp.
sea.program @const_start {
  %z = sea.const 9 : i8
  %mem = seaimp.var "mem" init %z shadow : !seaimp.var<!m>
  %n = sea.const 4 : i64
  %p = seaimp.alloca %mem, %n align 4 [1] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  seaimp.mem.fill %mem[%p], dense<[1, 2]> : tensor<2xi8> : !seaimp.var<!m>
  %c1 = sea.const 1 : i64
  %q = sea.ptr.add %p, %c1 : !sea.ptr<64>, i64
  %x = seaimp.load %mem[%q] : !seaimp.var<!m> -> i16
  // bytes 1, 2 = 2, then 9 (the constant start)
  %e = sea.const 2306 : i16
  %ok = sea.icmp eq, %x, %e : i16
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
