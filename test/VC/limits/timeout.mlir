// --timeout and --mem-limit are accepted by every backend; with generous
// limits the instance is solved as usual (no solve-timeout / mem-limit).
// RUN: sea-vc %s --timeout=600 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --timeout=600 --solver=yices | FileCheck %s %}
// RUN: sea-vc %s --timeout=600 --mem-limit=64 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --timeout=600 --mem-limit=64 --solver=yices | FileCheck %s %}
// CHECK-NOT: solve-timeout
// CHECK-NOT: mem-limit
// CHECK: result: sat
// CHECK: replay: assert-failed
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
