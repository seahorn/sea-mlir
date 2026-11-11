// Range-relative edge draws (--fuzz-ranges, on by default): a value site
// that an assume bounds gets values from its range, biased to the ends.
// Here x is in [0, 1000000] by (ult x 1000001), y in [1, 64] by
// (ult (add y -1) 64), and z in [0, x + y] by (ule z (add x y)). The bug
// needs z at the top of its range with x near the top of its own: random
// 64-bit values pass none of the range assumes, and the dictionary has no
// value for z.
// RUN: sea-run %s --fuzz=3000 --seed=1 | FileCheck %s
// RUN: sea-run %s --fuzz=3000 --seed=1 --fuzz-ranges=false | FileCheck %s --check-prefix=OFF

// CHECK: status: assert-failed
// CHECK: fuzz.failure: 1 trial=
// CHECK: fuzz.ranges: 3 value-sites=3 assumes=3 skipped=0 draws={{[1-9][0-9]*}} mode=edge
// OFF-NOT: status:
// OFF: fuzz.trials: 3000
// OFF: fuzz.ranges: 3 value-sites=3 assumes=3 skipped=0 draws=0 mode=off
// OFF-NOT: fuzz.failure
sea.program @main {
  %x = sea.nondet "nd_u64" [0] : i64
  %k = sea.const 1000001 : i64
  %xin = sea.icmp ult, %x, %k : i64
  sea.assume %xin
  %y = sea.nondet "nd_u64" [1] : i64
  %m1 = sea.const -1 : i64
  %y1 = sea.add %y, %m1 : i64
  %c64 = sea.const 64 : i64
  %yin = sea.icmp ult, %y1, %c64 : i64
  sea.assume %yin
  %s = sea.add %x, %y : i64
  %z = sea.nondet "nd_u64" [2] : i64
  %zin = sea.icmp ule, %z, %s : i64
  sea.assume %zin
  // the bug: z == x + y > 1000000
  %top = sea.icmp eq, %z, %s : i64
  %big = sea.icmp ugt, %s, %k : i64
  %bug = sea.and %top, %big : i1
  %t = sea.const true
  %ok = sea.xor %bug, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
