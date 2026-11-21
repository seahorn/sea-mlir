// sea-vc --check-inb lowers sea.box.all first (docs/design/CheckInb.md):
// the loads of a box's body become ordinary loads at the box's place,
// each with a bounds check; a part is sat iff some index of the box reads
// out of bounds. The claim's own assert is dropped.
// One arbitrary index (17 indices, above the expansion limit):
// RUN: sea-vc %s --program=last_oob --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=OOB
// Every index (expanded):
// RUN: sea-vc %s --program=last_oob --check-inb --lower --seam --ackermannize=off --box-all-expand=32 | FileCheck %s --check-prefix=OOB
// RUN: %if yices %{ sea-vc %s --program=last_oob --check-inb --lower --seam --ackermannize=off --solver=yices | FileCheck %s --check-prefix=OOB %}
// RUN: sea-vc %s --program=all_inb --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=INB
// RUN: sea-vc %s --program=all_inb --check-inb --lower --seam --ackermannize=off --box-all-expand=0 | FileCheck %s --check-prefix=INB
// RUN: sea-vc %s --program=maybe_empty --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=INB
// RUN: %if yices %{ sea-vc %s --program=maybe_empty --check-inb --lower --seam --ackermannize=off --solver=yices | FileCheck %s --check-prefix=INB %}

!m = !sea.mem<!sea.ptr<64>, i8>

// a[i] for i in [0, 17) of a 16-byte object: only i = 16 is out of bounds.
// OOB: check-inb.part: {{.*}}check-inb-box.mlir:[[@LINE+10]]:{{[0-9]+}} result=sat status=0
// OOB: result: sat
sea.program @last_oob {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %c17 = sea.const 17 : i64
  %z = sea.const 0 : i8
  %all = sea.box.all (%i) = (0) to (%c17) {
    %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %x = sea.load %m0[%p] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// The same box over [0, 16): every index in bounds.
// INB: result: unsat
sea.program @all_inb {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i8
  %all = sea.box.all (%i) = (0) to (%c16) {
    %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %x = sea.load %m0[%p] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// A box that may be empty: n bytes of an object of n bytes, n <= 16. With
// n = 0 the body does not run (a read at a + 0 would be out of bounds):
// no false alarm.
sea.program @maybe_empty {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.nondet "n" [1] : i64
  %c16 = sea.const 16 : i64
  %le = sea.icmp ule, %n, %c16 : i64
  sea.assume %le
  %a = sea.malloc %n align 8 [2] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i8
  %all = sea.box.all (%i) = (0) to (%n) {
    %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %x = sea.load %m0[%p] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
