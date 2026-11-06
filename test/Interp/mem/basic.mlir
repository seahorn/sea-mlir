// RUN: sea-run %s --program=store_load --missing=zero | FileCheck %s --check-prefix=SL
// RUN: sea-run %s --program=inputs --input=0@5=42 | FileCheck %s --check-prefix=IN
// RUN: not sea-run %s --program=inputs 2>&1 | FileCheck %s --check-prefix=MISSING
// RUN: sea-run %s --program=stale --missing=zero | FileCheck %s --check-prefix=STALE

!m8 = !sea.mem<!sea.addr<8>, i8>

// SL: status: halted
sea.program @store_load {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c3 = sea.const 3 : i8
  %a = sea.addr.from_int %c3 : i8 to !sea.addr<8>
  %v = sea.const 99 : i8
  %m1 = sea.store %m0[%a], %v inplace : !m8, i8
  %x = sea.load %m1[%a] : !m8 -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// A cell nobody wrote comes from the memory's site, once.
// IN: memread: site=0 addr=5 width=8 value=42 origin=input
// IN-NOT: memread
// IN: status: halted
// MISSING: message: no input for cell 5 of memory site 0
sea.program @inputs {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c5 = sea.const 5 : i8
  %a = sea.addr.from_int %c5 : i8 to !sea.addr<8>
  %x = sea.load %m0[%a] : !m8 -> i8
  %y = sea.load %m0[%a] : !m8 -> i8
  %c42 = sea.const 42 : i8
  %ok = sea.icmp eq, %y, %c42 : i8
  sea.assert %ok
  sea.halt
}

// A load of an earlier version reads the buffer as it is now (in place):
// correct only when the cell was not clobbered since that version.
// Here it was, so the load sees the new value; the VC would see the old
// one, and replay validation reports the mismatch.
// STALE: status: assert-failed
sea.program @stale {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c1 = sea.const 1 : i8
  %a = sea.addr.from_int %c1 : i8 to !sea.addr<8>
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%a], %v inplace : !m8, i8
  %old = sea.load %m0[%a] : !m8 -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %old, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
