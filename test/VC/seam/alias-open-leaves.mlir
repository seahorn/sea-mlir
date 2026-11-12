// The alias census (--seam-stats) classifies an open IS_OVERLAP by its leaf
// pairs, not by the view of a whole ite pointer (an ite has none).
// @loaded: a store through a loaded pointer p, a read at ite(c, A, B): two
// pairs (p, A), (p, B), each one-unknown. @noinb (--seam-inb=0): a store at
// B, a read at ite(c, A + k, A + j): two diff-object pairs, open because
// A-INB is off. The view of the whole ite said both-unknown / one-unknown.
// RUN: sea-vc %s --lower --program=loaded --seam --seam-stats --vcgen-only 2>&1 | FileCheck %s --check-prefix=LOADED
// RUN: sea-vc %s --lower --program=noinb --seam --seam-inb=0 --seam-stats --vcgen-only 2>&1 | FileCheck %s --check-prefix=NOINB
// LOADED: seam.alias.open.all: 1
// LOADED: seam.alias.open.one-unknown: 1
// LOADED: seam.alias.open-pairs.all: 2
// LOADED: seam.alias.open-pairs.one-unknown: 2
// NOINB: seam.alias.open.all: 1
// NOINB: seam.alias.open.diff-object: 1
// NOINB: seam.alias.open-pairs.all: 2
// NOINB: seam.alias.open-pairs.diff-object: 2
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @loaded {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %c = sea.nondet "c" [5] : i1
  %q = sea.select %c, %A, %B : !sea.ptr<64>
  %p = sea.load %m0[%C] : !m -> !sea.ptr<64>
  %y = sea.load %m0[%q] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %x = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @noinb {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %c = sea.nondet "c" [5] : i1
  %k = sea.nondet "k" [6] : i64
  %j = sea.nondet "j" [7] : i64
  %Ak = sea.ptr.add %A, %k : !sea.ptr<64>, i64
  %Aj = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %q = sea.select %c, %Ak, %Aj : !sea.ptr<64>
  %y = sea.load %m0[%q] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%B], %v inplace : !m, i8
  %x = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
