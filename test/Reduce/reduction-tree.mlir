// The sea patterns under upstream's reduction-tree (the dialect's
// DialectReductionPatternInterface): the test keeps a memmove and a sat
// verdict; loads, branches, assumes and the other updates go. The result
// verifies and is still sat. The opt-reduction-pass with sea-reduce-cleanup
// is all or nothing: here it would erase the dead memmove, so it is
// rejected and the dead ops stay (the driver still succeeds).
// RUN: printf '#!/bin/sh\ngrep -q sea.memmove "$2" || exit 0\n! "$1" "$2" 2>/dev/null | grep -q "^result: sat"\n' > %t.sh
// RUN: chmod +x %t.sh
// RUN: sea-reduce %s --reduction-tree='traversal-mode=0 test=%t.sh test-arg=%sea_vc' --opt-reduction-pass='opt-pass=sea-reduce-cleanup test=%t.sh test-arg=%sea_vc' -o %t.out 2>/dev/null
// RUN: FileCheck %s --input-file=%t.out
// sea-reduction-tree: the same search with the patterns only (no op
// deletion): the load becomes a nondet, the memmove stays.
// RUN: sea-reduce %s --sea-reduction-tree='test=%t.sh test-arg=%sea_vc' -o %t.sea 2>/dev/null
// RUN: FileCheck %s --check-prefix=SEA --input-file=%t.sea
// RUN: %sea_vc %t.sea | FileCheck %s --check-prefix=SAT
// SEA-NOT: sea.load
// SEA: sea.memmove
// SEA-NOT: sea.load
// --sea-patterns=none: op deletion only; the loads stay.
// RUN: sea-reduce %s --sea-patterns=none --reduction-tree='traversal-mode=0 test=%t.sh test-arg=%sea_vc' -o %t.none 2>/dev/null
// RUN: FileCheck %s --check-prefix=NONE --input-file=%t.none
// NONE: sea.load
// NONE-NOT: sea.reduce
// RUN: %sea_vc %t.out | FileCheck %s --check-prefix=SAT

// CHECK-LABEL: sea.program @main
// CHECK-NOT: sea.load
// CHECK-NOT: sea.store
// CHECK-NOT: sea.cond_br
// CHECK-NOT: sea.assume
// CHECK:     sea.memmove
// CHECK-NOT: sea.load
// CHECK:     sea.assert
// SAT: result: sat

sea.program @main {
  %m0 = sea.mem.nondet "heap" [0] : !sea.mem<!sea.addr<8>, i8>
  %a = sea.nondet "nd" [1] : i8
  %c1 = sea.const 1 : i8
  %c4 = sea.const 4 : i8
  %b = sea.add %a, %c1 : i8
  %p = sea.addr.from_int %b : i8 to !sea.addr<8>
  %q = sea.addr.from_int %a : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%p], %a inplace : !sea.mem<!sea.addr<8>, i8>, i8
  %m2 = sea.memmove %m1[%q], %m1[%p], %c4 inplace : !sea.mem<!sea.addr<8>, i8>
  %x = sea.load %m2[%p] : !sea.mem<!sea.addr<8>, i8> -> i8
  %c = sea.icmp ult, %x, %c4 : i8
  sea.assume %c
  %d = sea.icmp ult, %a, %c1 : i8
  sea.cond_br %d, ^bb1(%x : i8), ^bb2(%a : i8)
^bb1(%u : i8):
  sea.br ^bb3(%u : i8)
^bb2(%v : i8):
  sea.br ^bb3(%v : i8)
^bb3(%w : i8):
  %z = sea.icmp ne, %w, %c1 : i8
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
