// RUN: not sea-opt %s --sea-scalarize-structs 2>&1 | FileCheck %s

// Struct values in a block not reachable from the entry are outside the
// pass's domain: a loud error, not leftover struct ops.
// CHECK: scalarize.unreachable
sea.program @main {
  sea.halt
^dead:
  %a = sea.const 1 : i8
  %s = sea.struct.create %a : i8 -> !sea.struct<i8>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
