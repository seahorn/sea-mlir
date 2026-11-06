// RUN: sea-opt %s --sea-scalarize-structs | FileCheck %s
// RUN: sea-run %s --input=0=5 --input=1=6 --input=2=1 | FileCheck %s --check-prefix=STATUS
// RUN: sea-opt %s --sea-scalarize-structs | sea-run - --input=0=5 --input=1=6 --input=2=1 | FileCheck %s --check-prefix=STATUS
// RUN: not sea-vc %s 2>&1 | FileCheck %s --check-prefix=RAW
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC

// Pins that structs are gone (a struct block argument becomes one argument
// per leaf); the values are fuzzed (utils/fuzz-vc.py).
// CHECK-LABEL: sea.program @main
// CHECK-NOT: !sea.struct
// CHECK: ^bb3(%{{.*}}: i32, %{{.*}}: i16, %{{.*}}: i16):
// CHECK-NOT: !sea.struct
// STATUS: status: assert-failed
// RAW: vc.struct
// VC: result: sat
// VC: replay: assert-failed
sea.program @main {
  %a = sea.nondet "nondet_u16" [0] : i16
  %b = sea.nondet "nondet_u32" [1] : i32
  %c = sea.nondet "nondet_bool" [2] : i1
  %in = sea.struct.create %a, %a : i16, i16 -> !sea.struct<i16, i16>
  %s = sea.struct.create %b, %in : i32, !sea.struct<i16, i16> -> !sea.struct<i32, !sea.struct<i16, i16>>
  sea.cond_br %c, ^t, ^f
^t:
  %b2 = sea.const 7 : i32
  %s2 = sea.struct.insert %b2, %s[0] : i32 into !sea.struct<i32, !sea.struct<i16, i16>>
  sea.br ^j(%s2 : !sea.struct<i32, !sea.struct<i16, i16>>)
^f:
  sea.br ^j(%s : !sea.struct<i32, !sea.struct<i16, i16>>)
^j(%m : !sea.struct<i32, !sea.struct<i16, i16>>):
  %sel = sea.select %c, %m, %s : !sea.struct<i32, !sea.struct<i16, i16>>
  %x = sea.struct.extract %sel[0] : !sea.struct<i32, !sea.struct<i16, i16>> -> i32
  %ok = sea.icmp eq, %x, %b : i32
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
