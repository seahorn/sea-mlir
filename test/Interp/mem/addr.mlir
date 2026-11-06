// RUN: sea-run %s --trace | FileCheck %s

// Address ops are bit-vector arithmetic mod 2^W.
sea.program @addr {
  %c250 = sea.const 250 : i8
  %c10 = sea.const 10 : i8
  %p = sea.addr.from_int %c250 : i8 to !sea.addr<8>
  // CHECK: value: sea.addr.add {{.*}} = 4
  %q = sea.addr.add %p, %c10 : !sea.addr<8>, i8
  // CHECK: value: sea.addr.diff {{.*}} = 10
  %d = sea.addr.diff %q, %p : !sea.addr<8> -> i8
  // CHECK: value: sea.addr.cmp {{.*}} = 1
  %lt = sea.addr.cmp ult, %q, %p : !sea.addr<8>
  // CHECK: value: sea.select {{.*}} = 4
  %s = sea.select %lt, %q, %p : !sea.addr<8>
  // CHECK: value: sea.addr.to_int {{.*}} = 4
  %i = sea.addr.to_int %s : !sea.addr<8> to i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
