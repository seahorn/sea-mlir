// malloc(SIZE_MAX) next to a global: sea-place-static's unchecked uint64
// arithmetic placed the heap object at 0x08048004 with size 2^64 - 1, wrapping
// onto the global; with --seam the store at h - 4 (= the global) was skipped:
// --lower sat, --lower --seam unsat (wrong). Placement now checks overflow:
// out of space, an error, never a wrapped object.
// RUN: not sea-vc %s --lower 2>&1 | FileCheck %s
// RUN: not sea-vc %s --lower --seam 2>&1 | FileCheck %s
// CHECK: no room for heap.1 (18446744073709551615 units) in a 64-bit address space{{.*}}[place.out-of-space]
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @mm {
  %g = sea.global @g size 4 align 4 : !sea.ptr<64>
  %n = sea.const -1 : i64
  %h = sea.malloc %n align 1 [1] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !pm
  %v7 = sea.const 7 : i8
  %off = sea.nondet "off" [7] : i64
  %m4 = sea.const -4 : i64
  %isoff = sea.icmp eq, %off, %m4 : i64
  sea.assume %isoff
  %p = sea.ptr.add %h, %off : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%p], %v7 inplace : !pm, i8
  %x = sea.load %m1[%g] : !pm -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
