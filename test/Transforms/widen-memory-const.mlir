// sea-widen-memory on a memory that starts as sea.mem.const: the cell value
// becomes the word of k copies of the byte (a constant: one constant; a
// value: concats), so the program verifies and keeps its meaning. @splat:
// a word read of the constant memory is the replicated byte (unsat with
// and without widening). @byte: the byte is a nondet value.
// RUN: sea-opt %s --sea-widen-memory="k=8" | FileCheck %s
// RUN: sea-vc %s --program=splat --lower --widen-memory=8 2>&1 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=splat --lower 2>&1 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=byte --lower --widen-memory=8 2>&1 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=byte --lower --widen-memory=8 --seam 2>&1 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-opt %s --sea-widen-memory="k=8" | sea-run - --program=byte --missing=random | FileCheck %s --check-prefix=INTERP

// UNSAT: result: unsat
// INTERP: status: halted

!m = !sea.mem<!sea.ptr<64>, i8>

// CHECK-LABEL: sea.program @splat
// CHECK: %[[W:[0-9]+]] = sea.const -6148914691236517206 : i64
// CHECK: sea.mem.const %[[W]] : !sea.mem<!sea.ptr<64>, i64, aligned>
sea.program @splat {
  %z = sea.const 170 : i8
  %m = sea.mem.const %z : !m
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [0] : i64 -> !sea.ptr<64>
  %v = sea.load %m[%p] align 8 : !m -> i64
  %c = sea.const -6148914691236517206 : i64
  %e = sea.icmp eq, %v, %c : i64
  sea.assert %e
  sea.halt
}

// CHECK-LABEL: sea.program @byte
// CHECK: %[[B:[0-9]+]] = sea.nondet
// CHECK: %[[H:[0-9]+]] = sea.concat %[[B]], %[[B]] : i8, i8 -> i16
// CHECK: %[[S:[0-9]+]] = sea.concat %[[H]], %[[H]] : i16, i16 -> i32
// CHECK: %[[W:[0-9]+]] = sea.concat %[[S]], %[[S]] : i32, i32 -> i64
// CHECK: sea.mem.const %[[W]] : !sea.mem<!sea.ptr<64>, i64, aligned>
sea.program @byte {
  %b = sea.nondet "b" [1] : i8
  %m = sea.mem.const %b : !m
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [0] : i64 -> !sea.ptr<64>
  %eight = sea.const 8 : i64
  %q = sea.ptr.add %p, %eight : !sea.ptr<64>, i64
  %v = sea.load %m[%q] align 8 : !m -> i64
  %t = sea.trunc %v : i64 to i8
  %e = sea.icmp eq, %t, %b : i8
  %s = sea.const 56 : i64
  %hi = sea.lshr %v, %s : i64
  %ht = sea.trunc %hi : i64 to i8
  %f = sea.icmp eq, %ht, %b : i8
  %ok = sea.and %e, %f : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
