// RUN: not sea-vc %s --program=two_asserts 2>&1 | FileCheck %s --check-prefix=TWO
// RUN: not sea-vc %s --program=not_last 2>&1 | FileCheck %s --check-prefix=LAST
// RUN: not sea-vc %s --program=loop 2>&1 | FileCheck %s --check-prefix=LOOP
// RUN: not sea-vc %s --program=critical 2>&1 | FileCheck %s --check-prefix=CRIT
// RUN: not sea-vc %s --program=same_target 2>&1 | FileCheck %s --check-prefix=SAME

// TWO: expected exactly one sea.assert, found 2
sea.program @two_asserts {
  %t = sea.const 1 : i1
  sea.assert %t
  sea.assert %t
  sea.halt
}

// LAST: sea.assert must be immediately before sea.halt
sea.program @not_last {
  %t = sea.const 1 : i1
  sea.assert %t
  sea.assume %t
  sea.halt
}

// LOOP: the program has a loop
sea.program @loop {
  %b = sea.nondet "nondet_bool" [0] : i1
  sea.br ^head
^head:
  sea.cond_br %b, ^head, ^exit
^exit:
  sea.assert %b
  sea.halt
}

// CRIT: critical edge
sea.program @critical {
  %b = sea.nondet "nondet_bool" [0] : i1
  sea.cond_br %b, ^mid, ^exit
^mid:
  sea.br ^exit
^exit:
  sea.assert %b
  sea.halt
}

// SAME: sea.cond_br targets must be distinct
sea.program @same_target {
  %b = sea.nondet "nondet_bool" [0] : i1
  sea.cond_br %b, ^exit, ^exit
^exit:
  sea.assert %b
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
