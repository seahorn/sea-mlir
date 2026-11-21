// RUN: not sea-opt %s --sea-split-asserts 2>&1 | FileCheck %s
// RUN: not sea-opt %s --sea-split-asserts=group=2 2>&1 | FileCheck %s --check-prefix=GROUP

// Groups of asserts per part: a hook, not built yet.
// GROUP: Error: {{.*}}group=2 is not implemented{{.*}}[split-asserts.group]

// The assert's block on a cycle: an error.
// CHECK: Error: {{.*}}[split-asserts.cycle]
sea.program @loop {
  sea.br ^h
^h:
  %b = sea.nondet "b" [0] : i1
  sea.assert %b
  sea.cond_br %b, ^h, ^e
^e:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
