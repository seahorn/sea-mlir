// RUN: sea-run %s --missing=zero | FileCheck %s
// RUN: sea-opt %s --sea-lower-fat-pointers --sea-place-static | sea-run - --missing=zero | FileCheck %s
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC

// sea.ptr.nondet: a pointer that can never be dereferenced (a fresh object
// of size 0): not null, distinct from other objects, no byte in bounds.
// CHECK: status: halted
// VC: result: unsat
sea.program @nondet_ptr {
  %p = sea.ptr.nondet "nd_voidp" [0] : !sea.ptr<64>
  %n = sea.const 8 : i64
  %a = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %null = sea.ptr.null : !sea.ptr<64>
  %one = sea.const 1 : i64
  %d = sea.ptr.dereferenceable %p, %one : !sea.ptr<64>, i64
  %ne = sea.ptr.cmp ne, %p, %null : !sea.ptr<64>
  %na = sea.ptr.cmp ne, %p, %a : !sea.ptr<64>
  %t = sea.const 1 : i1
  %nd = sea.xor %d, %t : i1
  %r = sea.and %nd, %ne : i1
  %ok = sea.and %r, %na : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
