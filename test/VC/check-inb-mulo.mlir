// The lowering follows the parts' solver: with Yices (the default of the
// parts) umulo is expanded, not kept for z3; the parts are decided, not
// vc.intrinsic.
// RUN: %if yices %{ sea-vc %s --check-inb --lower --seam --ackermannize=off 2>&1 | FileCheck %s %}
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --solver=z3 2>&1 | FileCheck %s --check-prefix=Z3
// CHECK: check-inb.solver: yices (default)
// CHECK-NOT: vc.intrinsic
// CHECK: result: {{sat|unsat}}
// Z3: check-inb.solver: z3 (--solver)
// Z3-NOT: vc.intrinsic
// Z3: result: {{sat|unsat}}

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ag {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %k = sea.nondet "k" [3] : i64
  %o = sea.umulo %n, %k : i64
  %t = sea.const 1 : i1
  %no = sea.xor %o, %t : i1
  sea.assume %no
  %r = sea.mul %n, %k : i64
  %p = sea.ptr.add %a, %r : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
