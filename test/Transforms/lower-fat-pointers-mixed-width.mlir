// RUN: split-file %s %t
// RUN: not sea-opt %t/wider.mlir --sea-lower-fat-pointers 2>&1 | FileCheck %s --check-prefix=WIDER
// RUN: not sea-opt %t/narrower.mlir --sea-lower-fat-pointers 2>&1 | FileCheck %s --check-prefix=NARROWER
// RUN: not sea-vc %t/wider.mlir --lower 2>&1 | FileCheck %s --check-prefix=WIDER

// A companion cell has the memory's index width, so a pointer loaded or
// stored must have that width: a mixed width is an explicit error of the
// pass, before any change (not invalid IR left for the verifier).
// WIDER: Error: {{.*}}64-bit pointer in a memory indexed by 32-bit addresses {{.*}}wider.mlir":8:9) [fat.mixed-pointer-width]
// WIDER: Error: {{.*}}64-bit pointer in a memory indexed by 32-bit addresses {{.*}}wider.mlir":9:8) [fat.mixed-pointer-width]
// WIDER-NOT: keyed memory
// NARROWER: Error: {{.*}}32-bit pointer in a memory indexed by 64-bit addresses {{.*}}narrower.mlir":8:9) [fat.mixed-pointer-width]
// NARROWER-NOT: keyed memory

//--- wider.mlir
!m = !sea.mem<!sea.ptr<32>, i8>
sea.program @main {
  %m = sea.mem.nondet "m" [0] : !m
  %n = sea.const 16 : i32
  %p = sea.alloca %n align 8 [1] : i32 -> !sea.ptr<32>
  %c = sea.const 8 : i64
  %q = sea.alloca %c align 8 [2] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m[%p], %q inplace : !m, !sea.ptr<64>
  %r = sea.load %m1[%p] : !m -> !sea.ptr<64>
  %one = sea.const 1 : i64
  %d = sea.ptr.dereferenceable %r, %one : !sea.ptr<64>, i64
  sea.assert %d
  sea.halt
}

//--- narrower.mlir
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m = sea.mem.nondet "m" [0] : !m
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %c = sea.const 8 : i32
  %q = sea.alloca %c align 8 [2] : i32 -> !sea.ptr<32>
  %m1 = sea.store %m[%p], %q inplace : !m, !sea.ptr<32>
  %one = sea.const 1 : i32
  %d = sea.ptr.dereferenceable %q, %one : !sea.ptr<32>, i32
  sea.assert %d
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
