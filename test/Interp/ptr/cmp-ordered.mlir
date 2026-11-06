// RUN: sea-run %s --program=inside --input=2=15 | FileCheck %s --check-prefix=HALT
// RUN: sea-run %s --program=inside --missing=zero | FileCheck %s --check-prefix=HALT
// RUN: sea-run %s --program=before --missing=zero 2>/dev/null | FileCheck %s --check-prefix=HALT
// RUN: sea-run %s --program=before --missing=zero 2>&1 >/dev/null | FileCheck %s --check-prefix=OUTSIDE
// RUN: sea-vc %s --program=before --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-run %s --program=signed --missing=zero | FileCheck %s --check-prefix=HALT
// RUN: sea-vc %s --program=signed --lower | FileCheck %s --check-prefix=UNSAT
// RUN: not sea-run %s --program=two --missing=zero | FileCheck %s --check-prefix=TWO

// An ordered sea.ptr.cmp of abstract pointers compares their addresses as
// the VC does (ADDR after placement): same object, the offsets in address
// order. @inside: A + i <u
// A + 16 for i <u 16. @before: A - 1 <u A, as every placement has it (the
// interpreter warns: outside the object the order depends on placement).
// @signed: A <s A + 8. @two: different objects have no order before
// placement: an error.
// HALT: status: halted
// OUTSIDE: interp.ptr-cmp-outside
// UNSAT: result: unsat
// TWO: status: error
// TWO: message: ordered ptr.cmp of pointers into different objects
sea.program @inside {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %lt = sea.icmp ult, %i, %c16 : i64
  sea.assume %lt
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %e = sea.ptr.add %A, %c16 : !sea.ptr<64>, i64
  %ok = sea.ptr.cmp ult, %p, %e : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}
sea.program @before {
  %c16 = sea.const 16 : i64
  %m1 = sea.const -1 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %p = sea.ptr.add %A, %m1 : !sea.ptr<64>, i64
  %ok = sea.ptr.cmp ult, %p, %A : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}
sea.program @signed {
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %p = sea.ptr.add %A, %c8 : !sea.ptr<64>, i64
  %ok = sea.ptr.cmp slt, %A, %p : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}
sea.program @two {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %ok = sea.ptr.cmp ult, %A, %B : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
