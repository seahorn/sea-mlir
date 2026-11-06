// RUN: sea-opt %s --split-input-file --canonicalize | FileCheck %s

// The selected edge forwards its operands, not those of the dead edge.
// CHECK-LABEL: sea.program @take_true
// CHECK: %[[T:.*]] = sea.nondet "taken" [0] : i1
// CHECK-NOT: sea.cond_br
// CHECK: sea.br ^{{.*}}(%[[T]] : i1)
// CHECK-NOT: sea.cond_br
sea.program @take_true {
  %c = sea.const true
  %x = sea.nondet "taken" [0] : i1
  %y = sea.nondet "other" [1] : i1
  sea.cond_br %c, ^yes(%x : i1), ^no(%y : i1)
^yes(%a: i1):
  sea.assert %a
  sea.halt
^no(%b: i1):
  sea.assert %b
  sea.halt
}

// -----
// CHECK-LABEL: sea.program @take_false
// CHECK: %[[F:.*]] = sea.nondet "taken" [1] : i1
// CHECK-NOT: sea.cond_br
// CHECK: sea.br ^{{.*}}(%[[F]] : i1)
// CHECK-NOT: sea.cond_br
sea.program @take_false {
  %c = sea.const false
  %x = sea.nondet "other" [0] : i1
  %y = sea.nondet "taken" [1] : i1
  sea.cond_br %c, ^yes(%x : i1), ^no(%y : i1)
^yes(%a: i1):
  sea.assert %a
  sea.halt
^no(%b: i1):
  sea.assert %b
  sea.halt
}

// -----
// CHECK-LABEL: sea.program @same_edge
// CHECK: %[[S:.*]] = sea.nondet "value" [1] : i1
// CHECK-NOT: sea.cond_br
// CHECK: sea.br ^{{.*}}(%[[S]] : i1)
// CHECK-NOT: sea.cond_br
sea.program @same_edge {
  %c = sea.nondet "condition" [0] : i1
  %x = sea.nondet "value" [1] : i1
  sea.cond_br %c, ^join(%x : i1), ^join(%x : i1)
^join(%a: i1):
  sea.assert %a
  sea.halt
}

// -----
// Equal destinations alone do not make the edges equivalent.
// CHECK-LABEL: sea.program @different_operands
// CHECK: sea.cond_br
sea.program @different_operands {
  %c = sea.nondet "condition" [0] : i1
  %x = sea.nondet "x" [1] : i1
  %y = sea.nondet "y" [2] : i1
  sea.cond_br %c, ^join(%x : i1), ^join(%y : i1)
^join(%a: i1):
  sea.assert %a
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
