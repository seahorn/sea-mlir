// --timeout expiring: factoring a 122-bit number (two ~61-bit factors) is
// out of reach in a second: result unknown, solve-timeout: true, status 3.
// RUN: not sea-vc %s --timeout=1 2>&1 | FileCheck %s
// RUN: %if yices %{ not sea-vc %s --timeout=1 --solver=yices 2>&1 | FileCheck %s %}
// CHECK: result: unknown
// CHECK: solve-timeout: true
sea.program @factor {
  %x = sea.nondet "nx" [0] : i128
  %y = sea.nondet "ny" [1] : i128
  %c1 = sea.const 1 : i128
  %lim = sea.const 4611686018427387904 : i128
  %a = sea.icmp ugt, %x, %c1 : i128
  %b = sea.icmp ugt, %y, %c1 : i128
  %c = sea.icmp ult, %x, %lim : i128
  %d = sea.icmp ult, %y, %lim : i128
  sea.assume %a
  sea.assume %b
  sea.assume %c
  sea.assume %d
  %p = sea.mul %x, %y : i128
  %n = sea.const 5316911983139663422034469018046464031 : i128
  %ok = sea.icmp ne, %p, %n : i128
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
