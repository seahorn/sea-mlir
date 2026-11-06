// RUN: sea-run %s --input=0=1 | FileCheck %s --check-prefix=T
// RUN: sea-run %s --input=0=0 | FileCheck %s --check-prefix=F

// A diamond with a block argument: y = c ? 1 : 2, then assert y == 1.
// T: status: halted
// F: status: assert-failed
sea.program @diamond {
  %c = sea.nondet "nondet_bool" [0] : i1
  sea.cond_br %c, ^then, ^else
^then:
  %one = sea.const 1 : i32
  sea.br ^join(%one : i32)
^else:
  %two = sea.const 2 : i32
  sea.br ^join(%two : i32)
^join(%y : i32):
  %k = sea.const 1 : i32
  %ok = sea.icmp eq, %y, %k : i32
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
