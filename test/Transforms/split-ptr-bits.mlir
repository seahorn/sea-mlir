// RUN: sea-opt %s --sea-split-wide-access | FileCheck %s --check-prefix=SPLIT
// RUN: sea-opt %s | sea-opt | FileCheck %s --check-prefix=RT
// RUN: sea-run %s --program=bits --missing=zero | FileCheck %s --check-prefix=INT
// RUN: sea-opt %s --sea-split-wide-access | sea-run - --program=bits --missing=zero | FileCheck %s --check-prefix=INT
// RUN: sea-vc %s --program=bits --lower | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --program=bits --lower --seam | FileCheck %s --check-prefix=VC

// docs/design/PtrSort.md D2: sea-split-wide-access
// splits a pointer store with addr.to_bits and reassembles a pointer load
// with addr.from_bits, the pointer's own bits (its metadata lives in the
// companions at the address), not an int-to-ptr: the program stays in
// opaque mode.
// SPLIT: sea.addr.to_bits
// SPLIT-NOT: sea.addr.to_int
// SPLIT: sea.addr.from_bits
// SPLIT-NOT: sea.addr.from_int
// RT: sea.addr.from_bits {{.*}} : i8 to !sea.addr<8>
// RT: sea.addr.to_bits {{.*}} : !sea.addr<8> to i8
// INT: status: halted
// VC: vc.ptr-mode: opaque
// VC: result: unsat
!m = !sea.mem<!sea.addr<16>, i8>
sea.program @bits {
  %m0 = sea.mem.nondet "m" [0] : !m
  %a = sea.const 0x1234 : i16
  %p = sea.addr.from_bits %a : i16 to !sea.addr<16>
  %c = sea.const 0x40 : i16
  %q = sea.addr.from_bits %c : i16 to !sea.addr<16>
  %m1 = sea.store %m0[%q], %p inplace : !m, !sea.addr<16>
  %r = sea.load %m1[%q] : !m -> !sea.addr<16>
  %ri = sea.addr.to_bits %r : !sea.addr<16> to i16
  %ok = sea.icmp eq, %ri, %a : i16
  sea.assert %ok
  sea.halt
}
sea.program @rt {
  %x = sea.const 3 : i8
  %p = sea.addr.from_bits %x : i8 to !sea.addr<8>
  %y = sea.addr.to_bits %p : !sea.addr<8> to i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
