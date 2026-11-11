// --fuzz-reachable: block coverage against the reachable blocks of
// sea-vc --reach=blocks on the same (BMC-form) file. ^dead is unreachable
// (x < 4 and x > 10); ^rare (x == 200) is reachable and three trials miss
// it.
// RUN: rm -rf %t && mkdir -p %t
// RUN: sea-vc %s --reach=blocks > %t/reach.txt
// RUN: sea-run %s --fuzz=3 --seed=1 --fuzz-reachable=%t/reach.txt | FileCheck %s
// RUN: sea-run %s --fuzz=3 --seed=1 --fuzz-reachable=%t/reach.txt --format=jsonl > %t/out.jsonl
// RUN: sea-run %s --fuzz=3 --seed=1 --fuzz-reachable=%t/reach.txt > %t/out.txt
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t/out.txt --jsonl %t/out.jsonl
// An id of another program, a checks-kind file, no reach lines: errors.
// RUN: echo 'reach: other.mlir:1:1 reachable op=block at=other.mlir:1:1' > %t/other.txt
// RUN: not sea-run %s --fuzz=3 --fuzz-reachable=%t/other.txt 2>&1 | FileCheck %s --check-prefix=OTHER
// RUN: echo 'reach.config: kind=checks solver=incremental' > %t/checks.txt
// RUN: not sea-run %s --fuzz=3 --fuzz-reachable=%t/checks.txt 2>&1 | FileCheck %s --check-prefix=CHECKS
// RUN: echo 'result: unsat' > %t/none.txt
// RUN: not sea-run %s --fuzz=3 --fuzz-reachable=%t/none.txt 2>&1 | FileCheck %s --check-prefix=NONE
// A covered block that the file calls unreachable: a bug of one of the two.
// RUN: sed 's/ reachable op=block/ unreachable op=block/' %t/reach.txt > %t/bad.txt
// RUN: not sea-run %s --fuzz=3 --seed=1 --fuzz-reachable=%t/bad.txt 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not sea-run %s --fuzz=3 --defer-memory-errors --fuzz-reachable=%t/reach.txt 2>&1 | FileCheck %s --check-prefix=DEFER

// CHECK: fuzz.reachable: covered=6 reachable=7 unreachable=1 unknown=0 unknown-covered=0 unlisted=0
// CHECK-NEXT: fuzz.uncovered: {{.*}}reachable.mlir:[[@LINE+25]]:3 at=
// CHECK-NOT: fuzz.uncovered:
// OTHER: block 'other.mlir:1:1' is not a block of this program
// CHECKS: not the output of sea-vc --reach=blocks
// NONE: has no `reach:` lines
// BAD: which sea-vc --reach calls unreachable [fuzz.reach-inconsistent]
// DEFER: not with --defer-memory-errors
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c4 = sea.const 4 : i8
  %c10 = sea.const 10 : i8
  %c200 = sea.const 200 : i8
  %lt = sea.icmp ult, %x, %c4 : i8
  sea.cond_br %lt, ^a, ^b
^a:
  %gt = sea.icmp ugt, %x, %c10 : i8
  sea.cond_br %gt, ^dead, ^a2
^dead:
  sea.br ^j
^a2:
  sea.br ^j
^b:
  %eq = sea.icmp eq, %x, %c200 : i8
  sea.cond_br %eq, ^rare, ^b2
^rare:
  sea.br ^j
^b2:
  sea.br ^j
^j:
  %t = sea.const 1 : i1
  sea.assert %t
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
