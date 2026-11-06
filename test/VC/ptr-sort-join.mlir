// The pointer sort without SeaM (PtrSort S2g): addresses are ptr(w) in every
// VC but the lambda encoding; the boundary erases them. A pointer block
// argument joins by pointer equality under --vc-join=eq (Equality refuses
// ptr(w)), by ite under the default.
// RUN: sea-vc %s --vc-join=eq | FileCheck %s
// RUN: sea-vc %s | FileCheck %s
// RUN: sea-vc %s --vc-join=eq --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// CHECK: result: sat
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
// SMT: (declare-fun {{.*}} () (_ BitVec 64))
// SMT-NOT: ptr
sea.program @p {
  %c = sea.nondet "c" [0] : i1
  %a = sea.segment "a" base 4096 size 8 align 8 kind "stack" : !sea.addr<64>
  %b = sea.segment "b" base 8192 size 8 align 8 kind "stack" : !sea.addr<64>
  sea.cond_br %c, ^t, ^e
^t:
  sea.br ^j(%a : !sea.addr<64>)
^e:
  sea.br ^j(%b : !sea.addr<64>)
^j(%p : !sea.addr<64>):
  %ok = sea.addr.cmp ne, %p, %a : !sea.addr<64>
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
