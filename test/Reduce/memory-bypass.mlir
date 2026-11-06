// memory-bypass: an update's result becomes its incoming memory; once per
// update kind (store, memset, memmove, memhavoc), in program order. The
// output verifies (second sea-opt).
// RUN: sea-opt %s --sea-reduce-apply="pattern=memory-bypass limit=1" | sea-opt | FileCheck %s --check-prefix=ONE
// RUN: sea-opt %s --sea-reduce-apply="pattern=memory-bypass" | sea-opt | FileCheck %s --check-prefix=ALL
// report lists the sites (op indices in program order); op-index rewrites
// one of them (the audit of utils/reduce-test.py).
// RUN: sea-opt %s --sea-reduce-apply="pattern=memory-bypass report=true" -o /dev/null 2>&1 | FileCheck %s --check-prefix=REPORT
// RUN: sea-opt %s --sea-reduce-apply="pattern=memory-bypass op-index=6" | sea-opt | FileCheck %s --check-prefix=IDX
// REPORT: sea-reduce-apply.match: index=4 op=sea.store
// REPORT: sea-reduce-apply.match: index=7 op=sea.memhavoc
// IDX:     sea.store
// IDX:     sea.memset
// IDX-NOT: sea.memmove
// IDX:     sea.memhavoc

// ONE-LABEL: sea.program @main
// ONE:     %[[M0:.*]] = sea.mem.nondet
// ONE-NOT: sea.store
// ONE:     %[[M2:.*]] = sea.memset %[[M0]][
// ONE:     sea.memmove %[[M2]][
// ONE:     sea.memhavoc

// ALL-LABEL: sea.program @main
// ALL:     %[[M0:.*]] = sea.mem.nondet
// ALL-NOT: sea.store
// ALL-NOT: sea.memset
// ALL-NOT: sea.memmove
// ALL-NOT: sea.memhavoc
// ALL:     sea.load %[[M0]][

sea.program @main {
  %m0 = sea.mem.nondet "heap" [0] : !sea.mem<!sea.addr<8>, i8>
  %a = sea.nondet "nd" [1] : i8
  %p = sea.addr.from_int %a : i8 to !sea.addr<8>
  %c4 = sea.const 4 : i8
  %m1 = sea.store %m0[%p], %a inplace : !sea.mem<!sea.addr<8>, i8>, i8
  %m2 = sea.memset %m1[%p], %a, %c4 inplace : !sea.mem<!sea.addr<8>, i8>
  %m3 = sea.memmove %m2[%p], %m2[%p], %c4 inplace : !sea.mem<!sea.addr<8>, i8>
  %m4 = sea.memhavoc %m3[%p], %c4 [2] inplace : !sea.mem<!sea.addr<8>, i8>
  %x = sea.load %m4[%p] : !sea.mem<!sea.addr<8>, i8> -> i8
  %z = sea.icmp ne, %x, %a : i8
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
