// RUN: sea-run %s --input=0=0x1234567 | FileCheck %s
// RUN: sea-opt %s --sea-scalarize-structs | sea-run - --input=0=0x1234567 | FileCheck %s

// Pins the leaf layout of sea.struct.nondet (low bits first, in leaf order,
// pointer leaves null) in the interpreter and after scalarization:
// 0x1234567 = i16 0x4567, then i8 0x23, then i1 1 (bit 24).
// CHECK: status: halted
sea.program @main {
  %s = sea.struct.nondet "undef" [0] : !sea.struct<i16, !sea.ptr<32>, !sea.struct<i8, i1>>
  %x = sea.struct.extract %s[0] : !sea.struct<i16, !sea.ptr<32>, !sea.struct<i8, i1>> -> i16
  %p = sea.struct.extract %s[1] : !sea.struct<i16, !sea.ptr<32>, !sea.struct<i8, i1>> -> !sea.ptr<32>
  %n = sea.struct.extract %s[2] : !sea.struct<i16, !sea.ptr<32>, !sea.struct<i8, i1>> -> !sea.struct<i8, i1>
  %y = sea.struct.extract %n[0] : !sea.struct<i8, i1> -> i8
  %z = sea.struct.extract %n[1] : !sea.struct<i8, i1> -> i1
  %cx = sea.const 0x4567 : i16
  %cy = sea.const 0x23 : i8
  %null = sea.ptr.null : !sea.ptr<32>
  %ex = sea.icmp eq, %x, %cx : i16
  %ey = sea.icmp eq, %y, %cy : i8
  %ep = sea.ptr.cmp eq, %p, %null : !sea.ptr<32>
  %a = sea.and %ex, %ey : i1
  %b = sea.and %a, %ep : i1
  %ok = sea.and %b, %z : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
