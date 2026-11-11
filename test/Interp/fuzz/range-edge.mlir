// Range-relative edge draws (--fuzz-ranges, on by default): a value site
// that an assume bounds gets values from its range, biased to the ends.
// Here x is in [0, 1000000] by (ult x 1000001), y in [1, 64] by
// (ult (add y -1) 64), and z in [0, x + y] by (ule z (add x y)). The bug
// needs z at the top of its range with x near the top of its own: random
// 64-bit values pass none of the range assumes, and the dictionary has no
// value for z.
//
// By default each trial picks range draws or the generator without ranges
// with probability 1/2 (--fuzz-range-share), from the seeded stream: the
// same seed gives the same trials, and the failure file replays.
// RUN: rm -rf %t && mkdir -p %t
// RUN: sea-run %s --fuzz=3000 --seed=1 --fuzz-out=%t/mix | FileCheck %s
// RUN: sea-run %s --fuzz=3000 --seed=1 --fuzz-out=%t/mix2 > /dev/null
// RUN: diff -r %t/mix %t/mix2
// RUN: %python -c "import glob, shutil, sys; shutil.copy(*glob.glob(sys.argv[1]), sys.argv[2])" '%t/mix/failure-*.txt' %t/failure.txt
// RUN: sea-run %s --input-file=%t/failure.txt | FileCheck %s --check-prefix=REPLAY
// RUN: sea-run %s --fuzz=3000 --seed=1 --fuzz-range-share=1 | FileCheck %s --check-prefix=EDGE
// RUN: sea-run %s --fuzz=3000 --seed=1 --fuzz-ranges=false --fuzz-out=%t/off | FileCheck %s --check-prefix=OFF
// RUN: sea-run %s --fuzz=3000 --seed=1 --fuzz-range-share=0 --fuzz-out=%t/zero | FileCheck %s --check-prefix=ZERO
// RUN: diff -r %t/off %t/zero
// RUN: not sea-run %s --fuzz=5 --fuzz-range-share=1.5 2>&1 | FileCheck %s --check-prefix=SHARE
// RUN: not sea-run %s --fuzz=5 --fuzz-range-share=0.5 --fuzz-ranges=false 2>&1 | FileCheck %s --check-prefix=BOTH
// RUN: not sea-run %s --fuzz-range-share=0.5 2>&1 | FileCheck %s --check-prefix=NEEDS

// CHECK: status: assert-failed
// CHECK: fuzz.failure: 1 trial=
// CHECK: fuzz.ranges: 3 value-sites=3 assumes=3 skipped=0 draws={{[1-9][0-9]*}} mode=mix share=0.5 range-trials={{[1-9][0-9]*}}
// REPLAY: status: assert-failed
// EDGE: status: assert-failed
// EDGE: fuzz.trials: [[N:[0-9]+]]
// EDGE: fuzz.ranges: 3 value-sites=3 assumes=3 skipped=0 draws={{[1-9][0-9]*}} mode=edge share=1 range-trials=[[N]]
// OFF-NOT: status:
// OFF: fuzz.trials: 3000
// OFF: fuzz.ranges: 3 value-sites=3 assumes=3 skipped=0 draws=0 mode=off share=0 range-trials=0
// OFF-NOT: fuzz.failure
// SHARE: --fuzz-range-share must be in [0, 1]
// BOTH: --fuzz-range-share does not go with --fuzz-ranges=false
// NEEDS: --fuzz-range-share {{.*}}need --fuzz or --fuzz-time
// ZERO-NOT: status:
// ZERO: fuzz.ranges: 3 value-sites=3 assumes=3 skipped=0 draws=0 mode=off share=0 range-trials=0
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
