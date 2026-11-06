// RUN: sea-opt %s --sea-apply-tracking | FileCheck %s --check-prefix=APPLY
// RUN: sea-opt %s --sea-apply-tracking | sea-run - --program=track --missing=zero | FileCheck %s --check-prefix=OK
// RUN: not sea-run %s --program=track --missing=zero 2>&1 | FileCheck %s --check-prefix=MARK
// RUN: sea-opt %s --sea-apply-tracking --sea-lower-tracking | FileCheck %s --check-prefix=LOW
// RUN: sea-opt %s --sea-apply-tracking --sea-lower-tracking | sea-run - --program=track --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-apply-tracking --sea-lower-tracking | sea-run - --program=join --input=0=1 | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-apply-tracking --sea-lower-tracking | sea-run - --program=join --input=0=0 | FileCheck %s --check-prefix=OK

!m = !sea.mem<!sea.ptr<64>, i8>

// Updates between tracking_on and tracking_off are tracked; is_modified is
// a property of the object, until reset_modified.
// APPLY-LABEL: sea.program @track
// APPLY: sea.store {{.*}} inplace : !sea.mem
// APPLY: sea.store {{.*}} inplace tracked :
// APPLY: sea.memset {{.*}} inplace tracked :
// APPLY: sea.store {{.*}} inplace : !sea.mem
// APPLY-NOT: tracking_
// MARK: tracking markers must be applied first (sea-apply-tracking)
// OK: status: halted
// sea-lower-tracking: a shadow chain keyed by object base, no tracking left.
// LOW-LABEL: sea.program @track
// LOW: sea.mem.const %{{.*}} : !sea.mem<!sea.ptr<64>, i8>
// LOW-NOT: tracked
// LOW-NOT: is_modified
// LOW-NOT: reset_modified
sea.program @track {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 8 : i64
  %a = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %n align 8 [2] : i64 -> !sea.ptr<64>
  %c = sea.alloca %n align 8 [3] : i64 -> !sea.ptr<64>
  %v = sea.const 1 : i8
  // before tracking: not counted
  %m1 = sea.store %m0[%c], %v inplace : !m, i8
  sea.tracking_on
  %four = sea.const 4 : i64
  %a4 = sea.ptr.add %a, %four : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%a4], %v inplace : !m, i8
  %zero = sea.const 0 : i64
  // a zero-length memset writes nothing
  %m3 = sea.memset %m2[%b], %v, %zero inplace : !m
  sea.tracking_off
  // after tracking: not counted
  %m4 = sea.store %m3[%b], %v inplace : !m, i8
  %ma = sea.mem.is_modified %m4[%a] : !m
  %mb = sea.mem.is_modified %m4[%b] : !m
  %mc = sea.mem.is_modified %m4[%c] : !m
  %m5a = sea.mem.reset_modified %m4[%a4] inplace : !m
  %m5 = sea.mem.reset_modified %m5a[%a] inplace : !m
  %ma2 = sea.mem.is_modified %m5[%a] : !m
  %t = sea.const 1 : i1
  %f = sea.const 0 : i1
  %k1 = sea.icmp eq, %ma, %t : i1
  %k2 = sea.icmp eq, %mb, %f : i1
  %k3 = sea.icmp eq, %mc, %f : i1
  %k4 = sea.icmp eq, %ma2, %f : i1
  %r1 = sea.and %k1, %k2 : i1
  %r2 = sea.and %r1, %k3 : i1
  %ok = sea.and %r2, %k4 : i1
  sea.assert %ok
  sea.halt
}

// The shadow follows the data memory through a join: the object is modified
// on one branch only.
sea.program @join {
  %m0 = sea.mem.nondet "m" [1] : !m
  %n = sea.const 8 : i64
  %a = sea.alloca %n align 8 [2] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [0] : i1
  %v = sea.const 1 : i8
  sea.tracking_on
  sea.cond_br %c, ^write, ^join(%m0 : !m)
^write:
  %m1 = sea.store %m0[%a], %v inplace : !m, i8
  sea.br ^join(%m1 : !m)
^join(%m2: !m):
  sea.tracking_off
  %mod = sea.mem.is_modified %m2[%a] : !m
  %ok = sea.icmp eq, %mod, %c : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
