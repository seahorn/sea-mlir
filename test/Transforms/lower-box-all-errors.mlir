// RUN: not sea-opt %s --sea-lower-box-all --split-input-file 2>&1 | FileCheck %s
// RUN: not sea-opt %s --sea-lower-box-all --split-input-file --log=lower.box-all:1 2>&1 | FileCheck %s --check-prefix=LOG
// sea-lower-box-all refuses a claim whose result is not only asserted
// (through i1 and / or): no partial lowering, no fallback; the same for
// boxes small enough to expand (these run with the default expand-max)
// (docs/design/BoxAll.md). One error per op, by reason.
// LOG-DAG: refused-use-assume=1
// LOG-DAG: refused-use-negation=2

// CHECK-DAG: [box-all.use-assume]
sea.program @assume {
  %c4 = sea.const 4 : i64
  %all = sea.box.all (%i) = (0) to (%c4) {
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.assume %all
  %t = sea.const 1 : i1
  sea.assert %t
  sea.halt
}

// -----

// CHECK-DAG: [box-all.use-branch]
sea.program @branch {
  %c4 = sea.const 4 : i64
  %all = sea.box.all (%i) = (0) to (%c4) {
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.cond_br %all, ^a, ^b
^a:
  sea.halt
^b:
  sea.halt
}

// -----

// The negation of the claim (xor with 1, icmp eq 0), also after an and:
// CHECK-DAG: [box-all.use-negation]
sea.program @negation {
  %c4 = sea.const 4 : i64
  %t = sea.const 1 : i1
  %z = sea.const 0 : i1
  %all = sea.box.all (%i) = (0) to (%c4) {
    sea.yield %t
  }
  %all2 = sea.box.all (%i) = (0) to (%c4) {
    sea.yield %t
  }
  %n = sea.xor %all, %t : i1
  %a = sea.and %all2, %t : i1
  %n2 = sea.icmp eq, %a, %z : i1
  %ok = sea.and %n, %n2 : i1
  sea.assert %ok
  sea.halt
}

// -----

// CHECK-DAG: [box-all.use-select]
sea.program @select {
  %c4 = sea.const 4 : i64
  %t = sea.const 1 : i1
  %all = sea.box.all (%i) = (0) to (%c4) {
    sea.yield %t
  }
  %s = sea.select %all, %t, %t : i1
  sea.assert %s
  sea.halt
}

// -----

// CHECK-DAG: [box-all.use-value]
sea.program @value {
  %c4 = sea.const 4 : i64
  %t = sea.const 1 : i1
  %all = sea.box.all (%i) = (0) to (%c4) {
    sea.yield %t
  }
  %w = sea.zext %all : i1 to i8
  %c1 = sea.const 1 : i8
  %ok = sea.icmp eq, %w, %c1 : i8
  sea.assert %ok
  sea.halt
}

// -----

// A strong print would show one index's value, not the claim's.
// CHECK-DAG: [box-all.use-print]
sea.program @print {
  %c4 = sea.const 4 : i64
  %t = sea.const 1 : i1
  %all = sea.box.all (%i) = (0) to (%c4) {
    sea.yield %t
  }
  sea.print "all" %all : i1
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
