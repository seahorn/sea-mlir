// RUN: sea-vc %s --program=bounded --lower --check-ptr-index | FileCheck %s --check-prefix=HOLDS
// RUN: sea-vc %s --program=bounded --lower --check-ptr-index --seam | FileCheck %s --check-prefix=HOLDS
// RUN: sea-vc %s --program=unbounded --lower --check-ptr-index 2>&1 | FileCheck %s --check-prefix=BROKEN
// RUN: sea-vc %s --program=unbounded --lower --check-ptr-index --lower-opt=false 2>&1 | FileCheck %s --check-prefixes=BROKEN,WARN
// RUN: sea-vc %s --program=guarded --lower --check-ptr-index | FileCheck %s --check-prefix=HOLDS
// RUN: sea-vc %s --program=split --lower --check-ptr-index --seam | FileCheck %s --check-prefix=HOLDS
// RUN: not sea-vc %s --program=bounded --lower --check-ptr-index --check-align 2>&1 | FileCheck %s --check-prefix=BOTH

// sea-check-ptr-index (docs/design/PtrIndex.md section 6): the paranoid job
// of the trusted GEP flags. Each access whose address comes from an
// inbounds / nuw sea.ptr.index asserts that GEP's promise (P1-P3); the
// program's own asserts are dropped. @bounded: i < 3 in a 96-byte object,
// the promise holds (unsat). @unbounded: any i, a product wraps (sat; the
// interpreter warns the same at replay, when the lowering keeps the GEP: the
// job drops the program's assert, so the load and the GEP are dead and
// --lower-opt erases them). @guarded: a select of two GEPs, one
// with an index that may wrap, but only selected when it is small. @split:
// an 8-byte load, split into bytes (ptr.add of a constant to the GEP: still
// checked), bounded.
// HOLDS: result: unsat
// WARN: interp.ptr-index-promise
// BROKEN: result: sat
// BROKEN: validate: checked={{[0-9]+}} mismatches=0
// BOTH: vc.check-jobs
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @bounded {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %three = sea.const 3 : i64
  %iok = sea.icmp ult, %i, %three : i64
  sea.assume %iok
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds nuw : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}
sea.program @unbounded {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}
sea.program @guarded {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %three = sea.const 3 : i64
  %small = sea.icmp ult, %i, %three : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [] strides [] offset 16 inbounds : !sea.ptr<64>
  %q = sea.select %small, %a, %b : !sea.ptr<64>
  %x = sea.load %m0[%q] : !pm -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}
sea.program @split {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %three = sea.const 3 : i64
  %iok = sea.icmp ult, %i, %three : i64
  sea.assume %iok
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i64
  %z = sea.const 0 : i64
  %ok = sea.icmp eq, %x, %z : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
