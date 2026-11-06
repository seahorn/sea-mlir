// RUN: sea-vc %s --program=plain | FileCheck %s --check-prefix=OPAQUE
// RUN: sea-vc %s --program=cast | FileCheck %s --check-prefix=ADDR
// RUN: sea-vc %s --program=cast --ptr-mode=address | FileCheck %s --check-prefix=ADDR
// RUN: not sea-vc %s --program=cast --ptr-mode=opaque 2>&1 | FileCheck %s --check-prefix=REFUSE
// RUN: sea-vc %s --program=cast --seam | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --program=plain --seam | FileCheck %s --check-prefix=SEAM0

// --ptr-mode (docs/design/PtrSort.md): two pointer
// models, kept apart. A program with an int-to-ptr is in address mode
// (auto); opaque mode refuses one. With --seam the encoding reads the mode
// (S2f): addr.from_int is PTR_FROM_INT in address mode, counted as
// vc.ptr-from-int-opaque (a pointer with no provenance).
// OPAQUE: vc.ptr-mode: opaque
// OPAQUE: vc.int-to-ptr: 0
// ADDR: vc.ptr-mode: address
// ADDR: vc.int-to-ptr: 1
// REFUSE: vc.int-to-ptr
// SEAM: vc.ptr-mode: address
// SEAM: vc.ptr-from-int-opaque: 1
// SEAM: result: unsat
// SEAM0: vc.ptr-from-int-opaque: 0
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @plain {
  %x = sea.nondet "x" [0] : i8
  %y = sea.xor %x, %x : i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %y, %z : i8
  sea.assert %ok
  sea.halt
}
sea.program @cast {
  %x = sea.nondet "x" [0] : i8
  %a = sea.addr.from_int %x : i8 to !sea.addr<8>
  %b = sea.addr.to_int %a : !sea.addr<8> to i8
  %ok = sea.icmp eq, %x, %b : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
