// RUN: sea-run %s --missing=zero 2>&1 >/dev/null | FileCheck %s
// RUN: sea-run %s --missing=zero --log=interp.load-earlier-version 2>&1 >/dev/null | FileCheck %s --check-prefix=EACH

// A repeated warning prints once, then is summarized at exit.
// CHECK: Warning: load of an earlier memory version
// CHECK-SAME: [interp.load-earlier-version]
// CHECK-NOT: Warning: load of an earlier memory version
// CHECK: Warning: [interp.load-earlier-version] occurred 3 times
// CHECK-SAME: --log=interp.load-earlier-version shows each

// With --log=<kind>, every occurrence prints.
// EACH-COUNT-3: Warning: load of an earlier memory version
// EACH: occurred 3 times

!m8 = !sea.mem<!sea.addr<8>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%a], %z inplace : !m8, i8
  %x = sea.load %m0[%a] : !m8 -> i8
  %y = sea.load %m0[%a] : !m8 -> i8
  %w = sea.load %m0[%a] : !m8 -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
