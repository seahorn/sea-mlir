// A static out-of-object access keeps the premises (A-SEP, A-NULL,
// A-NOWRAP): a 4-byte store goes into a 1-byte alloca and overwrites the
// neighbour b, so the assertion on b fails (plain: sat). The static check
// sees the access leave its object (numeral offset) and land in b: --seam
// warns, reports where it lands, and answers under the premises, which
// this access breaks when it executes: unsat, conditional on a premise the
// program breaks (the expected answer of the default since 0.2.0).
// --seam-out-of-object=fallback runs again without the premises: the plain
// answer, sat with a replayed counterexample.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --seam --seam-out-of-object=fallback 2>&1 | FileCheck %s --check-prefix=FALLBACK
// RUN: sea-vc %s --lower --seam --seam-premises=0 | FileCheck %s --check-prefix=NOINB
// PLAIN: result: sat
// SEAM: Warning: {{.*}} leave their object{{.*}} the premises stay{{.*}}[vc.seam-out-of-object]
// SEAM: seam.out-of-object-at: {{.*}}inb-static.mlir:{{[0-9:]+}} store: {{.*}} lands=object
// SEAM: seam.premises: assumed
// SEAM: seam.out-of-object: accesses={{[0-9]+}} object={{[1-9]}}
// SEAM: result: unsat
// FALLBACK: Warning: {{.*}}running without the premises{{.*}}[vc.seam-out-of-object]
// FALLBACK: seam.premises: off (fallback: an access leaves its object)
// FALLBACK: replay: assert-failed
// FALLBACK: result: sat
// NOINB: seam.premises: off
// NOINB: result: sat
sea.program @oob {
  %m = sea.mem.nondet "r" [0] : !sea.mem<!sea.ptr<32>, i8>
  %c1 = sea.const 1 : i32
  %a = sea.alloca %c1 align 1 [1] : i32 -> !sea.ptr<32>
  %b = sea.alloca %c1 align 1 [2] : i32 -> !sea.ptr<32>
  %v = sea.const 0 : i32
  %m1 = sea.store %m[%b], %v inplace align 1 : !sea.mem<!sea.ptr<32>, i8>, i32
  %w = sea.const 77 : i32
  %m2 = sea.store %m1[%a], %w inplace align 1 : !sea.mem<!sea.ptr<32>, i8>, i32
  %x = sea.load %m2[%b] align 1 : !sea.mem<!sea.ptr<32>, i8> -> i32
  %e = sea.icmp eq, %x, %v : i32
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
