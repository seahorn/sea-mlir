// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --sccp | FileCheck %s --check-prefix=SCCP

// A segment's result is the object's base address and survives constant
// propagation: it has no folder, or sccp would replace it by a numeral and
// the object a pointer comes from would be lost (SeaM).
// CHECK-LABEL: sea.program @base
// CHECK: %[[B:.*]] = sea.segment "stack.3" base 3221225456 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
// CHECK: sea.addr.add %[[B]], %{{.*}} : !sea.addr<64>, i64
// SCCP-LABEL: sea.program @base
// SCCP: %[[B:.*]] = sea.segment "stack.3"
// SCCP: sea.addr.add %[[B]],
// SCCP-NOT: sea.addr.from_int
sea.program @base {
  %b = sea.segment "stack.3" base 3221225456 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  %four = sea.const 4 : i64
  %q = sea.addr.add %b, %four : !sea.addr<64>, i64
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
