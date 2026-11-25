// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-neg=sub --smt2 2>/dev/null | tr '\n' ' ' | tr -s ' ' | FileCheck %s --implicit-check-not=bvnot --implicit-check-not=bvneg
// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-neg=sub | FileCheck %s --check-prefix=SOLVE
// RUN: %if yices %{ sea-vc %s --vc-canonical-sums --vc-canonical-sums-neg=sub --solver=yices | FileCheck %s --check-prefix=SOLVE %}
// RUN: not sea-vc %s --vc-canonical-sums-neg=sub 2>&1 | FileCheck %s --check-prefix=ALONE

// --vc-canonical-sums-neg=sub (docs/design/CanonicalSums.md, "The
// negation"): the negative leaves N of a sum are subtracted, (P + c) - N,
// instead of added inverted, (P + ~N) + c+1. x - y - z + 5 = (x + 5) -
// (y + z); x + 200w = x - 56w. No bvnot, no bvneg.
// CHECK-DAG: (bvsub (bvadd nd.0_0 #x05) (bvadd nd.1_0 nd.2_0))
// CHECK-DAG: (bvsub nd.0_0 (bvmul #x38 nd.3_0))
// SOLVE: vc.canonical-sums: on neg=sub
// SOLVE: replay: assert-failed
// SOLVE: validate: checked={{[0-9]+}} mismatches=0
// SOLVE: result: sat
// ALONE: --vc-canonical-sums-neg needs --vc-canonical-sums
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %y = sea.nondet "f" [1] : i8
  %z = sea.nondet "f" [2] : i8
  %w = sea.nondet "f" [3] : i8
  %c5 = sea.const 5 : i8
  %c7 = sea.const 7 : i8
  %c200 = sea.const 200 : i8
  %c9 = sea.const 9 : i8
  %a = sea.sub %x, %y : i8
  %b = sea.sub %a, %z : i8
  %s = sea.add %b, %c5 : i8
  %m = sea.mul %c200, %w : i8
  %t = sea.add %x, %m : i8
  %u = sea.icmp ult, %t, %c9 : i8
  sea.assume %u
  %ok = sea.icmp ult, %s, %c7 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
