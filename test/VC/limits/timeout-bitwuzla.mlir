// REQUIRES: bitwuzla
// --timeout / --mem-limit as Bitwuzla options (time-limit-per,
// memory-limit): accepted, a generous limit leaves the result as it is.
// RUN: sea-vc %s --timeout=600 --mem-limit=64 --solver=bitwuzla | FileCheck %s
// CHECK-NOT: solve-timeout
// CHECK-NOT: mem-limit
// CHECK: result: sat
sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %y = sea.add %x, %c100 : i8
  %ok = sea.icmp uge, %y, %c100 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
