// RUN: sea-opt --sea-split-at-checks %s | FileCheck %s
// sea-split-at-checks (sea-vc --reach): every assume / assert /
// sea.unreachable starts a block whose only predecessor branches to it; a
// block's first op too (the block keeps its arguments and predecessors).

// CHECK-LABEL: sea.program @main
// CHECK:         sea.icmp
// CHECK-NEXT:    sea.br ^[[A1:bb[0-9]+]]
// CHECK:       ^[[A1]]:
// CHECK-NEXT:    sea.assume
// CHECK-NEXT:    sea.br ^[[A2:bb[0-9]+]]
// CHECK:       ^[[A2]]:
// CHECK-NEXT:    sea.assume
// CHECK-NEXT:    sea.cond_br %{{.*}}, ^[[TA:bb[0-9]+]], ^
// CHECK:       ^[[TA]]:
// CHECK-NEXT:    sea.br ^[[TA2:bb[0-9]+]]
// CHECK:       ^[[TA2]]:
// CHECK-NEXT:    sea.assume
// CHECK:       ^{{bb[0-9]+}}(%{{.*}}: i8):
// CHECK-NEXT:    sea.icmp
// CHECK-NEXT:    sea.br ^[[A3:bb[0-9]+]]
// CHECK:       ^[[A3]]:
// CHECK-NEXT:    sea.assume
// CHECK:         sea.const false
// CHECK-NEXT:    sea.br ^[[A4:bb[0-9]+]]
// CHECK:       ^[[A4]]:
// CHECK-NEXT:    sea.assert
// CHECK-NEXT:    sea.halt
sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %z = sea.const 0 : i8
  %c = sea.icmp ne, %x, %z : i8
  sea.assume %c
  sea.assume %c
  sea.cond_br %c, ^a, ^b
^a:
  sea.assume %c
  sea.br ^j(%x : i8)
^b:
  sea.br ^j(%z : i8)
^j(%v: i8):
  %d = sea.icmp ne, %v, %z : i8
  sea.assume %d
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
