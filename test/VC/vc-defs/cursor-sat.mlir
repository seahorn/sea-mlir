// Named address sums (sea-vc --vc-defs, docs/design/SumNames.md) on a parser
// cursor: o_k = o_{k-1} + zext(x_{k-1}) + 2, a byte read at p + o_k each step.
// The erased addresses are flat sums with a common prefix. --vc-defs=nest
// emits each as its parent's sum plus the rest (one shared node, no name);
// hidden and eq name them: each address is a name def.<k>, defined from its
// parent's name. --smt2: hidden asserts each definition as two bvule atoms
// and no equation over a name; eq as one equation; inline (the default) has
// flat sums and no name.
// --ack-decider=false: the difference decider drops these pairs (their
// offsets are bounded); the test needs them constrained.
// RUN: sea-vc %s --lower --seam --ack-decider=false --smt2 --vc-defs=hidden 2>/dev/null | FileCheck %s --check-prefix=HID
// RUN: sea-vc %s --lower --seam --ack-decider=false --smt2 --vc-defs=eq 2>/dev/null | FileCheck %s --check-prefix=EQ
// RUN: sea-vc %s --lower --seam --ack-decider=false --smt2 2>/dev/null | FileCheck %s --check-prefix=INL
// RUN: sea-vc %s --lower --seam --ack-decider=false --smt2 --vc-defs=nest 2>/dev/null | FileCheck %s --check-prefix=NEST
// HID: (declare-fun def.0 () (_ BitVec 64))
// HID-NOT: (= def.{{[0-9]+$}}
// HID-DAG: (bvule def.1
// HID-DAG: (bvule (bvadd def.0
// HID-NOT: (= def.{{[0-9]+$}}
// EQ: (= def.{{[0-9]+$}}
// INL-NOT: {{def\.|\(bvadd \(bvadd}}
// NEST-NOT: def.
// NEST: (bvadd (bvadd obj.heap.1
// NEST-NOT: def.
// The names are counted, and a sat model replays and validates (the names
// are not program values) with every backend.
// RUN: sea-vc %s --lower --seam --ack-decider=false --vc-defs=hidden 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --ack-decider=false --vc-defs=eq 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --ack-decider=false --vc-defs=nest 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --ack-decider=false 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: %if yices %{ sea-vc %s --lower --seam --ack-decider=false --vc-defs=hidden --solver=yices 2>&1 | FileCheck %s %}
// RUN: %if bitwuzla %{ sea-vc %s --lower --seam --ack-decider=false --vc-defs=hidden --ackermannize=on --solver=bitwuzla 2>&1 | FileCheck %s %}
// CHECK-DAG: vc.defs-names: {{[1-9][0-9]*}}
// CHECK-DAG: result: sat
// CHECK-DAG: replay: assert-failed
// CHECK-DAG: validate: checked={{[0-9]+}} mismatches=0
// OFF: vc.defs: inline
// OFF-NOT: vc.defs-names
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @cursor {
  %m = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 4096 : i64
  %p = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %c2 = sea.const 2 : i64
  %o0 = sea.const 0 : i64
  %q0 = sea.ptr.add %p, %o0 : !sea.ptr<64>, i64
  %x0 = sea.load %m[%q0] : !m -> i8
  %z1 = sea.zext %x0 : i8 to i64
  %s1 = sea.add %o0, %z1 : i64
  %o1 = sea.add %s1, %c2 : i64
  %q1 = sea.ptr.add %p, %o1 : !sea.ptr<64>, i64
  %x1 = sea.load %m[%q1] : !m -> i8
  %z2 = sea.zext %x1 : i8 to i64
  %s2 = sea.add %o1, %z2 : i64
  %o2 = sea.add %s2, %c2 : i64
  %q2 = sea.ptr.add %p, %o2 : !sea.ptr<64>, i64
  %x2 = sea.load %m[%q2] : !m -> i8
  %z3 = sea.zext %x2 : i8 to i64
  %s3 = sea.add %o2, %z3 : i64
  %o3 = sea.add %s3, %c2 : i64
  %q3 = sea.ptr.add %p, %o3 : !sea.ptr<64>, i64
  %x3 = sea.load %m[%q3] : !m -> i8
  %z4 = sea.zext %x3 : i8 to i64
  %s4 = sea.add %o3, %z4 : i64
  %o4 = sea.add %s4, %c2 : i64
  %q4 = sea.ptr.add %p, %o4 : !sea.ptr<64>, i64
  %x4 = sea.load %m[%q4] : !m -> i8
  %z5 = sea.zext %x4 : i8 to i64
  %s5 = sea.add %o4, %z5 : i64
  %o5 = sea.add %s5, %c2 : i64
  %q5 = sea.ptr.add %p, %o5 : !sea.ptr<64>, i64
  %x5 = sea.load %m[%q5] : !m -> i8
  %c7 = sea.const 7 : i8
  %ok = sea.icmp ne, %x5, %c7 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
