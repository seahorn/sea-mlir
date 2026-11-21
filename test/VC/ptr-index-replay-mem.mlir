// RUN: sea-vc %s --lower 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s

// A GEP whose promise breaks is stored, loaded back and loaded through
// (docs/design/PtrIndex.md section 6): the counterexample replays on the
// lowered program, where sea-split-wide-access moves the pointer through
// memory as to_bits / extract / cell stores and cell loads / concat /
// from_bits. The replay warns at the load through the reloaded pointer.
// i is in [0, 2^63); the assertion fails for i >= 2^59, where 24 i breaks
// P1.
// CHECK-DAG: result: sat
// CHECK-DAG: the pointer went through memory, and this sea.load uses it as its address{{.*}}[interp.ptr-index-promise]
// CHECK-DAG: replay: assert-failed
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @viamem {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !pm
  %n = sea.const 96 : i64
  %c8 = sea.const 8 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %slot = sea.malloc %c8 align 8 [4] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %zero = sea.const 0 : i64
  %nonneg = sea.icmp sge, %i, %zero : i64
  sea.assume %nonneg
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %s1 = sea.store %m0[%slot], %a inplace align 8 : !pm, !sea.ptr<64>
  %r = sea.load %s1[%slot] align 8 : !pm -> !sea.ptr<64>
  %x = sea.load %s1[%r] : !pm -> i8
  %lim = sea.const 576460752303423488 : i64
  %small = sea.icmp ult, %i, %lim : i64
  %xz = sea.icmp eq, %x, %z : i8
  %ok = sea.and %small, %xz : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
