// sea-reduce runs upstream's reduction-tree on a sea program: the generic
// op deletion keeps what the test needs (a sat verdict) and drops the
// assume that does not matter; the result verifies and stays sat.
// RUN: printf '#!/bin/sh\n! "$1" "$2" | grep -q "^result: sat"\n' > %t.sh
// RUN: chmod +x %t.sh
// RUN: sea-reduce %s --reduction-tree='traversal-mode=0 test=%t.sh test-arg=%sea_vc' -o %t.out 2>%t.err
// RUN: FileCheck %s --input-file=%t.out
// RUN: FileCheck %s --check-prefix=ERR --input-file=%t.err
// RUN: %sea_vc %t.out | FileCheck %s --check-prefix=SAT
// Not interesting (unsat): the input is refused.
// RUN: printf '#!/bin/sh\n! "$1" "$2" | grep -q "^result: unsat"\n' > %t.unsat.sh
// RUN: chmod +x %t.unsat.sh
// RUN: not sea-reduce %s --reduction-tree='test=%t.unsat.sh test-arg=%sea_vc' 2>&1 | FileCheck %s --check-prefix=UNINT

// CHECK: sea.program
// CHECK-NOT: sea.assume
// CHECK: sea.assert
// ERR: sea-reduce.diagnostics-suppressed:
// ERR: sea-reduce.verifier-rejected:
// SAT: result: sat
// UNINT: uninterested module will not be reduced
// UNINT: sea-reduce: failed

sea.program @main {
  %m0 = sea.mem.nondet "heap" [0] : !sea.mem<!sea.addr<8>, i8>
  %a = sea.nondet "nd" [1] : i8
  %c1 = sea.const 1 : i8
  %b = sea.add %a, %c1 : i8
  %p = sea.addr.from_int %b : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%p], %a inplace : !sea.mem<!sea.addr<8>, i8>, i8
  %x = sea.load %m1[%p] : !sea.mem<!sea.addr<8>, i8> -> i8
  %c = sea.icmp eq, %x, %a : i8
  sea.assume %c
  %z = sea.icmp ne, %x, %c1 : i8
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
