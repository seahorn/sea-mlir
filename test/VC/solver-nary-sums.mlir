// RUN: sea-vc %s --simplify --solver-nary-sums --smt2 2>/dev/null | tr '\n' ' ' | tr -s ' ' | FileCheck %s
// RUN: sea-vc %s --simplify --solver-nary-sums --smt2 2>/dev/null | tr '\n' ' ' | tr -s ' ' | not grep -E '\(bv(add|mul|and|or|xor) nd\.[0-9]_0 \(bv'
// RUN: sea-vc %s --simplify --smt2 2>/dev/null | tr '\n' ' ' | tr -s ' ' | FileCheck %s --check-prefix=CHAIN
// RUN: sea-vc %s --simplify --solver-nary-sums --smt2 2>&1 >/dev/null | FileCheck %s --check-prefix=STAT
// RUN: sea-vc %s --simplify --solver-nary-sums | FileCheck %s --check-prefix=SOLVE
// RUN: sea-vc %s --simplify | FileCheck %s --check-prefix=SOLVE-OFF --implicit-check-not=vc.solver-nary-sums
// RUN: %if yices %{ sea-vc %s --simplify --solver-nary-sums --solver=yices | FileCheck %s --check-prefix=SOLVE %}
// RUN: %if bitwuzla %{ sea-vc %s --simplify --solver-nary-sums --solver=bitwuzla | FileCheck %s --check-prefix=SOLVE %}
// RUN: %if bitwuzla %{ sea-vc %s --simplify --solver-nary-sums --solver=bitwuzla --smt2 2>/dev/null | FileCheck %s --check-prefix=BZLA %}

// --solver-nary-sums: an n-ary bvadd, bvmul, bvand, bvor and bvxor of the
// query (--simplify makes x + z + w one node) reaches z3 as one n-ary app,
// the term z3's parser builds from the flat (bvadd a b c) of --smt2. The
// default right-nested chain shares its suffix (bvadd z w) between the two
// sums, so z3's printer writes it nested there.
// The two sums of each operator share their last two leaves.
// CHECK-DAG: (bvadd nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvadd nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvmul nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvmul nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvand nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvand nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvor nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvor nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvxor nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHECK-DAG: (bvxor nd.{{[0-9]}}_0 nd.{{[0-9]}}_0 nd.{{[0-9]}}_0)
// CHAIN: (bvadd nd.{{[0-9]}}_0 (bvadd nd.{{[0-9]}}_0 nd.{{[0-9]}}_0))
// STAT: vc.solver-nary-sums: on
// Bitwuzla's operators are binary: the left-associated chain in argument
// order, with or without the switch.
// BZLA: (bvadd (bvadd nd.{{[0-9]}}_0 nd.{{[0-9]}}_0) nd.{{[0-9]}}_0)
// The query keeps its meaning: the model replays.
// SOLVE: vc.solver-nary-sums: on
// SOLVE: replay: assert-failed
// SOLVE: validate: checked={{[0-9]+}} mismatches=0
// SOLVE: result: sat
// SOLVE-OFF: result: sat
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %y = sea.nondet "f" [1] : i8
  %z = sea.nondet "f" [2] : i8
  %w = sea.nondet "f" [3] : i8
  %c9 = sea.const 9 : i8
  %a = sea.add %x, %z : i8
  %s = sea.add %a, %w : i8
  %b = sea.add %y, %z : i8
  %t = sea.add %b, %w : i8
  %ma = sea.mul %x, %z : i8
  %ms = sea.mul %ma, %w : i8
  %mb = sea.mul %y, %z : i8
  %mt = sea.mul %mb, %w : i8
  %aa = sea.and %x, %z : i8
  %as = sea.and %aa, %w : i8
  %ab = sea.and %y, %z : i8
  %at = sea.and %ab, %w : i8
  %oa = sea.or %x, %z : i8
  %os = sea.or %oa, %w : i8
  %ob = sea.or %y, %z : i8
  %ot = sea.or %ob, %w : i8
  %xa = sea.xor %x, %z : i8
  %xs = sea.xor %xa, %w : i8
  %xb = sea.xor %y, %z : i8
  %xt = sea.xor %xb, %w : i8
  %u1 = sea.icmp ult, %t, %c9 : i8
  %u2 = sea.icmp ult, %mt, %ms : i8
  %u3 = sea.icmp ult, %at, %as : i8
  %u4 = sea.icmp ult, %ot, %os : i8
  %u5 = sea.icmp ult, %xt, %xs : i8
  %v1 = sea.and %u1, %u2 : i1
  %v2 = sea.and %u3, %u4 : i1
  %v3 = sea.and %v1, %v2 : i1
  %v = sea.and %v3, %u5 : i1
  sea.assume %v
  %ok = sea.icmp ult, %s, %c9 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
