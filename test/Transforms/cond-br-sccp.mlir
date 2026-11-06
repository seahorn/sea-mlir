// RUN: sea-opt %s --sccp | FileCheck %s --check-prefix=SCCP
// RUN: sea-opt %s --sccp --canonicalize | FileCheck %s --check-prefix=PRUNE

// Only the executable predecessor contributes to the join's constant value.
// SCCP: %[[TRUE:.*]] = sea.const true
// SCCP: sea.assert %[[TRUE]]
// PRUNE-NOT: sea.cond_br
// PRUNE-NOT: sea.nondet "dead"
// PRUNE: sea.assert
// PRUNE-NOT: sea.cond_br
// PRUNE-NOT: sea.nondet "dead"
sea.program @derived {
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  %c = sea.icmp ult, %one, %two : i8
  sea.cond_br %c, ^yes, ^no
^yes:
  sea.br ^join(%one : i8)
^no:
  %dead = sea.nondet "dead" [0] : i1
  sea.assert %dead
  sea.br ^join(%two : i8)
^join(%n: i8):
  %ok = sea.icmp eq, %n, %one : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
