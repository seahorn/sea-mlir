// The difference decider runs on the index the lowering compares, at its
// width (docs/design/Ackermann.md, "The difference decider"). Reads at
// q + zext(x & 7) and q + zext(y & 7) + 16 of a 16-byte object: as 64-bit
// addresses the difference lies in [-23, -9], never 0 (dropped). Under
// --narrow-offsets (k = 4) the pair compares its 4-bit offsets, whose
// difference ranges over [-7, 7] modulo 16: it may be 0, so the k-bit
// constraint stays (the second read is out of its object: A-INB, on which
// the narrowing rests, rules it out; the counts are the point here).
// RUN: sea-vc %s --lower --seam --vcgen-only 2>&1 | FileCheck %s --check-prefix=WIDE
// RUN: sea-vc %s --lower --seam --narrow-offsets --vcgen-only 2>&1 | FileCheck %s --check-prefix=NARROW
// WIDE: vc.ackermann-pairs: 1
// WIDE: vc.ackermann-decider-pairs: 1
// WIDE: vc.ackermann-constraints: 0
// NARROW: vc.ackermann-pairs: 1
// NARROW: vc.ackermann-decider-pairs: 0
// NARROW: vc.ackermann-constraints: 1
// NARROW: vc.narrow-pairs: 1
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @width {
  %m = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %q = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %x = sea.nondet "x" [2] : i8
  %y = sea.nondet "y" [3] : i8
  %c7 = sea.const 7 : i8
  %xm = sea.and %x, %c7 : i8
  %ym = sea.and %y, %c7 : i8
  %xo = sea.zext %xm : i8 to i64
  %yz = sea.zext %ym : i8 to i64
  %yo = sea.add %yz, %c16 : i64
  %pa = sea.ptr.add %q, %xo : !sea.ptr<64>, i64
  %pb = sea.ptr.add %q, %yo : !sea.ptr<64>, i64
  %a = sea.load %m[%pa] : !m -> i8
  %b = sea.load %m[%pb] : !m -> i8
  %ok = sea.icmp eq, %a, %b : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
