// --fuzz-progress=S: fuzz.progress lines (elapsed, then the totals so far)
// during the run, before the final lines; none by default. The first
// failure's line has its time.
// RUN: sea-run %s --program=safe --fuzz-time=0.35 --fuzz-progress=0.1 --seed=1 | FileCheck %s
// RUN: sea-run %s --program=safe --fuzz-time=0.35 --fuzz-progress=0.1 --seed=1 --format=jsonl | FileCheck %s --check-prefix=JSONL
// RUN: sea-run %s --program=safe --fuzz=1000 --seed=1 | FileCheck %s --check-prefix=OFF
// RUN: sea-run %s --program=safe --fuzz-time=0.25 --fuzz-progress=0.1 --seed=1 --fuzz-cmp=false | FileCheck %s --check-prefix=NOCMP
// RUN: sea-run %s --program=fail --fuzz=1000 --seed=1 | FileCheck %s --check-prefix=FAIL
// RUN: not sea-run %s --program=safe --fuzz-progress=1 2>&1 | FileCheck %s --check-prefix=NOFUZZ
// RUN: not sea-run %s --program=safe --fuzz-coverage-out=%t.cov 2>&1 | FileCheck %s --check-prefix=NOFUZZ
// RUN: not sea-run %s --program=safe --fuzz=10 --fuzz-progress=-1 2>&1 | FileCheck %s --check-prefix=NEG

// CHECK: fuzz.progress: {{0\.[1-9][0-9]*}} trials={{[1-9][0-9]*}} steps={{[1-9][0-9]*}} blocks=4 edges=5 paths=3 assumes=1 failures=0 frontier=1 cmp-buckets={{[1-9][0-9]*}}{{$}}
// NOCMP: fuzz.progress: {{.*}} frontier=1{{$}}
// CHECK: fuzz.trials:

// JSONL: {"kind":"stat","key":"fuzz.progress","value":{{[0-9.]+}},"trials":{{[0-9]+}},"steps":{{[0-9]+}},"blocks":{{[0-9]+}},"edges":{{[0-9]+}},"paths":{{[0-9]+}},"assumes":{{[0-9]+}},"failures":0,

// OFF-NOT: fuzz.progress
// OFF: fuzz.trials:
// OFF-NOT: fuzz.progress

// FAIL: fuzz.failure: 1 trial={{[0-9]+}} at={{.*}} time={{[0-9]+\.[0-9]+$}}

// NOFUZZ: need --fuzz or --fuzz-time
// NEG: --fuzz-progress must not be negative
sea.program @safe {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assume %lt
  %c5 = sea.const 5 : i8
  %small = sea.icmp ult, %x, %c5 : i8
  sea.cond_br %small, ^a, ^b
^a:
  sea.br ^done
^b:
  sea.br ^done
^done:
  sea.halt
}

sea.program @fail {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assert %lt
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
