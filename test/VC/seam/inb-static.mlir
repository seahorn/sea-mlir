// A-INB (accesses stay in their objects) is assumed by --seam. Here a 4-byte
// store goes into a 1-byte alloca and overwrites the neighbour b, so the
// assertion on b fails (sat). The static check sees the access leave its
// object (numeral offset): --seam warns and runs without the assumption,
// instead of answering unsat.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --seam --seam-inb=0 | FileCheck %s --check-prefix=NOINB
// PLAIN: result: sat
// SEAM: Warning: an access leaves its object{{.*}}[vc.seam-inb-violated]
// SEAM: seam.inb: off (fallback: an access leaves its object)
// SEAM: result: sat
// SEAM: replay: assert-failed
// NOINB: seam.inb: off
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
