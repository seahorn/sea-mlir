// A shallow bug: found within a fixed seed and budget, and its failure
// file replays through --input-file to the same failure.
// RUN: rm -rf %t && sea-run %s --fuzz=2000 --seed=1 --fuzz-out=%t > %t.out
// RUN: FileCheck %s < %t.out
// RUN: cat %t/failure-*.txt > %t.fail
// RUN: sea-run %s --input-file=%t.fail | FileCheck %s --check-prefix=REPLAY

// CHECK: status: assert-failed
// CHECK-NEXT: at: loc({{.*}}shallow.mlir":[[@LINE+16]]
// CHECK: fuzz.failure: 1 trial={{[0-9]+}} file={{.*}}failure-{{[0-9]+}}.txt at={{.*}}shallow.mlir:[[@LINE+15]]
// CHECK: fuzz.status: {{.*}} assert-failed=1 {{.*}} error=0
// REPLAY: origin=input
// REPLAY: status: assert-failed
// REPLAY-NEXT: at: loc({{.*}}shallow.mlir":[[@LINE+11]]
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %c100 = sea.const 100 : i8
  %c5 = sea.const 5 : i8
  %big = sea.icmp ugt, %x, %c100 : i8
  %small = sea.icmp ult, %y, %c5 : i8
  %both = sea.and %big, %small : i1
  %one = sea.const 1 : i1
  %ok = sea.xor %both, %one : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
