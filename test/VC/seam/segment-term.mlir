// An object under --seam is the term PTR_SEGMENT(k, a), its placed address
// in the term (docs/design/PtrSegment.md, step 1): printed
// segment(obj.<k>, 0x<a>) before the boundary (the rewriter's trace), and
// erased at the boundary to the constant obj.<k>, whose address the
// boundary states as a conjunct obj.<k> = a. @skip: a store to one object
// is skipped by a read of the other (A-SEP): the read is the earlier one,
// and the verdict is the plain encoding's. @open: the query keeps a read,
// so it has the layout conjuncts.
// RUN: sea-vc %s --program=skip --seam --seam-trace=row.store-skip 2>&1 | FileCheck %s --check-prefix=TRACE
// RUN: sea-vc %s --program=skip --seam | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --program=skip | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --program=open --seam --smt2 --vc-fold-top-eqs=false | FileCheck %s --check-prefix=SMT
// TRACE: seam-trace: {{.*}}rules=row.store-skip
// TRACE-NEXT: seam-trace-lhs: select(store({{.*}}, segment(obj.heap.2, 0x2000), {{.*}}), segment(obj.heap.1, 0x1000))
// TRACE-NEXT: seam-trace-rhs: select({{.*}}, segment(obj.heap.1, 0x1000))
// SMT-DAG: (declare-fun obj.heap.1 () (_ BitVec 32))
// SMT-DAG: (declare-fun obj.heap.2 () (_ BitVec 32))
// SMT-DAG: (= obj.heap.1 #x00001000)
// SMT-DAG: (= obj.heap.2 #x00002000)
// SMT: (check-sat)
// VERDICT: result: unsat
!am = !sea.mem<!sea.addr<32>, i8>
sea.program @skip {
^bb0:
  %a = sea.segment "heap.1" base 4096 : i32 size 16 : i32 align 16 kind "heap" : !sea.addr<32>
  %b = sea.segment "heap.2" base 8192 : i32 size 16 : i32 align 16 kind "heap" : !sea.addr<32>
  %m = sea.mem.nondet "m" [0] : !am
  %x0 = sea.load %m[%a] : !am -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m[%b], %v : !am, i8
  %x = sea.load %m1[%a] : !am -> i8
  %c = sea.icmp eq, %x, %x0 : i8
  sea.assert %c
  sea.halt
}

sea.program @open {
^bb0:
  %a = sea.segment "heap.1" base 4096 : i32 size 16 : i32 align 16 kind "heap" : !sea.addr<32>
  %b = sea.segment "heap.2" base 8192 : i32 size 16 : i32 align 16 kind "heap" : !sea.addr<32>
  %m = sea.mem.nondet "m" [0] : !am
  %v = sea.const 7 : i8
  %m1 = sea.store %m[%b], %v : !am, i8
  %x = sea.load %m1[%a] : !am -> i8
  %c = sea.icmp eq, %x, %v : i8
  sea.assert %c
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
