// A segment must not wrap the address space (base + size <= 2^w). Object
// disjointness compares linear intervals, which matches the circular address
// semantics only for such objects. @wrap's heap.1 = [250, 260) covered 0..3
// at 8-bit addresses and aliased heap.2 = [0, 2): plain sea-vc and sea-run
// fail the assert, but --seam skipped the store as "another object" and
// answered unsat (wrong). The verifier now rejects the program. segments-top.mlir: an object ending exactly at 2^w is fine.
// RUN: not sea-vc %s --seam 2>&1 | FileCheck %s --check-prefix=WRAP
// RUN: not sea-vc %s 2>&1 | FileCheck %s --check-prefix=WRAP
// WRAP: error: 'sea.segment' op the object wraps the address space (base + size > 2^8)
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @wrap {
  %a = sea.segment "heap.1" base 250 : i8 size 10 : i8 align 1 kind "heap" : !sea.addr<8>
  %b = sea.segment "heap.2" base 0 : i8 size 2 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v7 = sea.const 7 : i8
  %c6 = sea.const 6 : i8
  %p = sea.addr.add %a, %c6 : !sea.addr<8>, i8
  %m1 = sea.store %m0[%p], %v7 inplace : !am, i8
  %x = sea.load %m1[%b] : !am -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
