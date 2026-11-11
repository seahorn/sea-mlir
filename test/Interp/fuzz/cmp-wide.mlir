// --fuzz-cmp at the width limits: the eq distance is modulo 2^w the short
// way (x = 2^64 - 1 is 1 from 0); a distance past 2^64 - 1 is clamped; a
// signed difference of i64 operands does not wrap (bucket -L, not +L).
// RUN: rm -rf %t && mkdir -p %t/c
// RUN: echo '0=18446744073709551615' > %t/c/a.txt
// RUN: printf '0=0\n1=1267650600228229401496703205376\n' > %t/c/b.txt
// RUN: printf '0=0\n1=0\n2=9223372036854775808\n' > %t/c/c.txt
// RUN: sea-run %s --fuzz=3 --corpus=%t/c --fuzz-cmp --fuzz-gates \
// RUN:   --fuzz-coverage-out=%t/cov.txt | FileCheck %s
// RUN: FileCheck %s --check-prefix=FILE < %t/cov.txt

// CHECK: fuzz.gate: 0 op=sea.assume passed=2 blocked=1 distance=1 measured=1
// CHECK: fuzz.gate: 1 op=sea.assume passed=1 blocked=1 distance=18446744073709551615 measured=1
// CHECK: fuzz.gate: 2 op=sea.assume passed=0 blocked=1 distance=18446744073709551615 measured=1
// FILE: cmp: 0 pred=eq role=assume counts=0,0,0,2,0,0,1
// FILE: cmp: 1 pred=ult role=assume counts=0,1,0,0,0,0,1
// FILE: cmp: 2 pred=sgt role=assume counts=1,0,0,0,0,0,0
sea.program @main {
  %x = sea.nondet "nondet_u64" [0] : i64
  %z = sea.const 0 : i64
  %eq = sea.icmp eq, %x, %z : i64
  sea.assume %eq
  %y = sea.nondet "nondet_u128" [1] : i128
  %c5 = sea.const 5 : i128
  %lt = sea.icmp ult, %y, %c5 : i128
  sea.assume %lt
  %w = sea.nondet "nondet_i64" [2] : i64
  %max = sea.const 9223372036854775807 : i64
  %gt = sea.icmp sgt, %w, %max : i64
  sea.assume %gt
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
