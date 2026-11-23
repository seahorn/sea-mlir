// RUN: sea-vc %s --vc-canonical-sums --smt2 2>/dev/null | tr '\n' ' ' | tr -s ' ' | FileCheck %s --implicit-check-not=bvsub --implicit-check-not=bvneg
// RUN: sea-vc %s --vc-canonical-sums | FileCheck %s --check-prefix=SOLVE
// RUN: %if yices %{ sea-vc %s --vc-canonical-sums --solver=yices | FileCheck %s --check-prefix=SOLVE %}

// Item 1 of the form (docs/design/CanonicalSums.md): a sum is P, P + c or
// (P + ~N) + c+1, with no bvsub, no bvneg and no per-term negation; the
// constant comes last, folded. x - y - z + 5 = (x + ~(y + z)) + 6; a
// coefficient k > 2^(w-1) is the negative leaf 2^w - k: 200 * w = -56 * w.
// The smt2 printer of z3 writes nested bvadd flat; the text is joined into
// one line (the printer wraps long terms).
// CHECK-DAG: (bvadd nd.0_0 (bvnot (bvadd nd.1_0 nd.2_0)) #x06)
// CHECK-DAG: (bvadd nd.0_0 (bvnot (bvmul #x38 nd.3_0)) #x01)
// The VC keeps its meaning: the model replays (x - y - z + 5 = 7 is
// reachable).
// SOLVE: vc.canonical-sums: on
// SOLVE: vc.canonical-sums-sums: 2
// SOLVE: result: sat
// SOLVE: replay: assert-failed
// SOLVE: validate: checked={{[0-9]+}} mismatches=0
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
