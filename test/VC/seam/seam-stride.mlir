// RUN: sea-vc %s --lower --seam --seam-stats --seam-stride --seam-check | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-stats | FileCheck %s --check-prefix=OFF

// R-STRIDE (docs/design/PtrIndex.md section 0c): a header written at
// numeral offsets 8 and 16 of an object (a store-map), read through the
// slot GEP p + 24 i + 96 (inbounds). 24 divides neither 8 - 96 nor 16 - 96,
// so both entries are skipped (R-ABORT), under the GEP's promise
// (premise.ptr-index); --seam-check proves each firing under P1-P3.
// CHECK: seam.rule.premise.ptr-index: 2
// CHECK: seam.rule.row.smap-abort-stride: 2
// CHECK: result: unsat
// CHECK: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0 unknown=0 unexpressible=0
// Without the rule the answer is sat: an index that wraps (24 i = -80 mod
// 2^64, which breaks the promise) reads a header byte. The promise is what
// the rule adds; the interpreter warns interp.ptr-index-promise on replay.
// OFF-NOT: smap-abort-stride
// OFF: result: sat
// OFF: validate: checked={{[0-9]+}} mismatches=0
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @header {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 192 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %c8 = sea.const 8 : i64
  %c16 = sea.const 16 : i64
  %h1 = sea.ptr.add %p, %c8 : !sea.ptr<64>, i64
  %h2 = sea.ptr.add %p, %c16 : !sea.ptr<64>, i64
  %a = sea.const 1 : i8
  %b = sea.const 2 : i8
  %s = sea.ptr.index %p [%i] strides [24] offset 96 inbounds : !sea.ptr<64>, i64
  %y = sea.load %m0[%s] : !pm -> i8
  %m1 = sea.store %m0[%h1], %a inplace : !pm, i8
  %m2 = sea.store %m1[%h2], %b inplace : !pm, i8
  %x = sea.load %m2[%s] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
