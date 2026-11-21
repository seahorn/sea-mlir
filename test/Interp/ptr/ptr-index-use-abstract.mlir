// RUN: sea-run %s --program=abstract 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=absover 2>&1 | FileCheck %s --check-prefix=QUIET

// ptr-index-use.mlir for an abstract program (kept apart: the split RUN
// there needs placed programs). A broken GEP stored and loaded back is
// still poison: the cells of the pointer's pieces carry the mark, and an
// intact pointer stored over them clears it. 2^61 * 24 breaks P1.
// QUIET-NOT: interp.ptr-index-promise
// QUIET: status: halted
// MEM: sea.ptr.index at {{.*}} nusw: a product wraps (P1): the promise is broken (a poison pointer in LLVM), the pointer went through memory, and this sea.load uses it{{.*}}[interp.ptr-index-promise]

// an abstract program: the pointer's pieces carry the mark
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @abstract {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !pm
  %n = sea.const 96 : i64
  %c8 = sea.const 8 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %slot = sea.malloc %c8 align 8 [4] : i64 -> !sea.ptr<64>
  %i = sea.const 2305843009213693952 : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %s1 = sea.store %m0[%slot], %a inplace align 8 : !pm, !sea.ptr<64>
  %r = sea.load %s1[%slot] align 8 : !pm -> !sea.ptr<64>
  %x = sea.load %s1[%r] : !pm -> i8
  sea.halt
}
sea.program @absover {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !pm
  %n = sea.const 96 : i64
  %c8 = sea.const 8 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %slot = sea.malloc %c8 align 8 [4] : i64 -> !sea.ptr<64>
  %i = sea.const 2305843009213693952 : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %s1 = sea.store %m0[%slot], %a inplace align 8 : !pm, !sea.ptr<64>
  %s2 = sea.store %s1[%slot], %p inplace align 8 : !pm, !sea.ptr<64>
  %r = sea.load %s2[%slot] align 8 : !pm -> !sea.ptr<64>
  %x = sea.load %s2[%r] : !pm -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
