// sea-check-ptr-index follows GEPs, ptr.add / addr.add and select. Here the
// nusw GEP is stored to a slot and loaded back: the walk stops at the load,
// and says so (warning check-ptr-index.not-followed), so an "unsat" is not
// read as covering that GEP. The VC itself is unchanged.
// RUN: sea-vc %s --lower --check-ptr-index 2>&1 | FileCheck %s
// CHECK: comes from sea.load
// CHECK-SAME: [check-ptr-index.not-followed]
// CHECK: result: unsat
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @P {
  %a = sea.segment "heap.1" base 16 : i8 size 16 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %x = sea.nondet "x" [1] : i8
  %s = sea.segment "heap.2" base 48 : i8 size 1 : i8 align 1 kind "heap" : !sea.addr<8>
  %n = sea.ptr.index %a [%x] strides [3] offset 0 nusw : !sea.addr<8>, i8
  %pm1 = sea.store %m0[%s], %n inplace : !am, !sea.addr<8>
  %q = sea.load %pm1[%s] : !am -> !sea.addr<8>
  %y = sea.load %pm1[%q] : !am -> i8
  %ok = sea.icmp eq, %y, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
