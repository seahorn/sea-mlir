// RUN: sea-opt %s --sea-split-critical-edges | FileCheck %s
// RUN: sea-vc %s --program=critical --lower | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --program=same_target --lower | FileCheck %s --check-prefix=VC

// The edge's block arguments move to the new block's branch.
// CHECK-LABEL: sea.program @critical
// CHECK: sea.cond_br %{{.*}}, ^[[MID:bb[0-9]+]], ^[[EDGE:bb[0-9]+]]
// CHECK: ^[[MID]]:
// CHECK: ^[[EDGE]]:
// CHECK-NEXT: sea.br ^[[EXIT:bb[0-9]+]](%{{.*}} : i8)
// CHECK: ^[[EXIT]](
sea.program @critical {
  %b = sea.nondet "nondet_bool" [0] : i1
  %x = sea.nondet "nondet_u8" [1] : i8
  sea.cond_br %b, ^mid, ^exit(%x : i8)
^mid:
  %z = sea.const 0 : i8
  sea.br ^exit(%z : i8)
^exit(%v : i8):
  %c = sea.icmp eq, %v, %x : i8
  sea.assert %c
  sea.halt
}

// Both edges of a cond_br with equal targets are critical.
// CHECK-LABEL: sea.program @same_target
// CHECK: sea.cond_br %{{.*}}, ^[[A:bb[0-9]+]], ^[[B:bb[0-9]+]]
// CHECK: ^[[A]]:
// CHECK-NEXT: sea.br
// CHECK: ^[[B]]:
// CHECK-NEXT: sea.br
// VC: result: sat
// VC: replay: assert-failed
sea.program @same_target {
  %b = sea.nondet "nondet_bool" [0] : i1
  sea.cond_br %b, ^exit, ^exit
^exit:
  sea.assert %b
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
