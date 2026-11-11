// Model coverage of --fuzz: the assume frontier, the gates no trial passed,
// per-gate pass / block counts (--fuzz-gates), comparison buckets and branch
// distances (--fuzz-cmp). Only the seeds run (--fuzz=5, five seeds, in file
// order), so every count is known:
//   a: x=12        blocked at gate 0 (ult x 10: distance 12 - 10 + 1 = 3)
//   b: x=10        blocked at gate 0 (distance 1)
//   c: x=3 y=5     blocked at gate 1 (y != 5: distance 1)
//   d: x=3 y=4 z=90  blocked at gate 2 (sgt z 100: distance 100 - 90 + 1 = 11)
//   e: x=3 y=4 z=255 blocked at gate 2 (z = -1 signed: distance 102)
// RUN: rm -rf %t && mkdir -p %t/c
// RUN: echo '0=12' > %t/c/a.txt
// RUN: echo '0=10' > %t/c/b.txt
// RUN: printf '0=3\n1=5\n' > %t/c/c.txt
// RUN: printf '0=3\n1=4\n2=90\n' > %t/c/d.txt
// RUN: printf '0=3\n1=4\n2=255\n' > %t/c/e.txt
// RUN: sea-run %s --program=main --fuzz=5 --corpus=%t/c --fuzz-gates \
// RUN:   --fuzz-coverage-out=%t/cov.txt | FileCheck %s
// RUN: FileCheck %s --check-prefix=FILE < %t/cov.txt
// RUN: sea-run %s --program=main --fuzz=5 --corpus=%t/c --fuzz-cmp=false | FileCheck %s --check-prefix=OFF
// RUN: sea-run %s --program=main --fuzz=5 --corpus=%t/c | FileCheck %s --check-prefix=NOCHK
// RUN: echo '0=200' > %t/k.txt
// RUN: sea-run %s --program=check --fuzz=1 --corpus=%t/k.txt | FileCheck %s --check-prefix=CHECKED
// RUN: sea-run %s --program=main --fuzz=5 --corpus=%t/c --fuzz-cmp --format=jsonl > %t.jsonl
// RUN: sea-run %s --program=main --fuzz=5 --corpus=%t/c --fuzz-cmp > %t.txt
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl
// RUN: not sea-run %s --program=main --fuzz-cmp 2>&1 | FileCheck %s --check-prefix=NOFUZZ

// two trials passed two assumes; the first of them (d) ended at gate 2
// CHECK: fuzz.frontier: deepest=2 trial=4 end=assume-blocked gate=2 mean=1.00 assumes=3 passed=2 closed=1 unreached=0
// CHECK-NEXT: fuzz.gate-closed: 2 blocked=2 distance=11 at={{.*}}model-coverage.mlir:[[@LINE+52]]:3 sites=2 cond=(sgt s2 100)
// CHECK-NEXT: fuzz.gate: 0 op=sea.assume passed=3 blocked=2 distance=1 measured=2 at=
// CHECK-NEXT: fuzz.gate: 1 op=sea.assume passed=2 blocked=1 distance=1 measured=1 at=
// CHECK-NEXT: fuzz.gate: 2 op=sea.assume passed=0 blocked=2 distance=11 measured=2 at=
// gate 0: -S +S 0; gate 1: 0 -1; branch: -1; gate 2: -S -L
// CHECK-NEXT: fuzz.cmp: 4 reached=4 buckets=8/28 outcomes=6/8 assume=7/21 branch=1/7 value=0/0

// no assume of @main is directly followed by `assert false`
// NOCHK: fuzz.frontier:
// NOCHK-NOT: check=1
// NOCHK: fuzz.cmp:

// FILE: frontier: deepest=2 sum=5
// FILE: gate: 0 op=sea.assume passed=3 blocked=2 distance=1
// FILE: gate: 2 op=sea.assume passed=0 blocked=2 distance=11
// buckets -L -S -1 0 +1 +S +L
// FILE: cmp: 0 pred=ult role=assume counts=0,3,0,1,0,1,0 at=
// FILE: cmp: 1 pred=eq role=assume counts=0,0,2,1,0,0,0 at=
// FILE: cmp: 2 pred=ult role=branch counts=0,0,2,0,0,0,0 at=
// FILE: cmp: 3 pred=sgt role=assume counts=1,1,0,0,0,0,0 at=

// OFF: fuzz.frontier: deepest=2
// OFF-NEXT: fuzz.gate-closed: 2 blocked=2 at=
// OFF-NOT: distance=
// OFF-NOT: fuzz.cmp:

// @check: the one seed (x = 200) is blocked by the assume of `x < 10`,
// which is directly followed by `assert false`
// CHECKED: fuzz.gate-closed: 0 blocked=1 {{.*}}check=1

// NOFUZZ: --fuzz-cmp, --fuzz-gates and --fuzz-reachable need --fuzz
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assume %lt
  %y = sea.nondet "nondet_u8" [1] : i8
  %c5 = sea.const 5 : i8
  %t = sea.const 1 : i1
  %eq = sea.icmp eq, %y, %c5 : i8
  %ne = sea.xor %eq, %t : i1
  sea.assume %ne
  %b = sea.icmp ult, %y, %c5 : i8
  sea.cond_br %b, ^l, ^r
^l:
  sea.br ^j
^r:
  sea.br ^j
^j:
  %z = sea.nondet "nondet_u8" [2] : i8
  %c100 = sea.const 100 : i8
  %gt = sea.icmp sgt, %z, %c100 : i8
  sea.assume %gt
  sea.halt
}

sea.program @check {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assume %lt
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
