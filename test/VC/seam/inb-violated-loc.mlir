// vc.seam-inb-violated names the accesses behind the fallback: the store and
// the load at a numeral offset 12 of an 8-byte object, with the location and
// the check that failed; then the count.
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s
// CHECK: vc.seam-inb-violated: at {{.*}}inb-violated-loc.mlir:[[@LINE+13]]:{{[0-9]+}} store: numeral offset outside: offset 12
// CHECK: vc.seam-inb-violated: at {{.*}}inb-violated-loc.mlir:[[@LINE+13]]:{{[0-9]+}} load: numeral offset outside
// CHECK: vc.seam-inb-violated-count: 2
// CHECK: seam.inb: off
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @oob {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c8 = sea.const 8 : i64
  %a = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %c12 = sea.const 12 : i64
  %q = sea.ptr.add %a, %c12 : !sea.ptr<64>, i64
  %v = sea.const 9 : i8
  %m1 = sea.store %m0[%q], %v inplace : !m, i8
  %x = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
