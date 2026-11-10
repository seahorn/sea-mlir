// --progress is an alias of --log=vc.phase (--progress=false of
// --log=vc.phase:0): the start and end of each phase
// on stderr, in order, written as they happen; nothing without the flag. A
// later --log entry overrides it.
// RUN: sea-vc %s --lower --progress 2>&1 >/dev/null | FileCheck %s
// RUN: sea-vc %s --lower --log=vc.phase 2>&1 >/dev/null | FileCheck %s
// RUN: sea-vc %s --lower 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-vc %s --lower --progress --log=vc.phase:0 2>&1 | FileCheck %s --check-prefix=QUIET
// --progress=false turns it off, at its place among the --log entries.
// RUN: sea-vc %s --lower --log=vc.phase --progress=false 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-vc %s --lower --progress=false --log=vc.phase 2>&1 >/dev/null | FileCheck %s
// CHECK: log: vc.phase start phase=lower
// CHECK-NEXT: log: vc.phase end phase=lower time=
// CHECK-NEXT: log: vc.phase start phase=encode
// CHECK-NEXT: log: vc.phase end phase=encode time=
// CHECK-NEXT: log: vc.phase start phase=solver-term
// CHECK-NEXT: log: vc.phase end phase=solver-term time=
// CHECK-NEXT: log: vc.phase start phase=ackermann
// CHECK-NEXT: log: vc.phase end phase=ackermann time=
// CHECK-NEXT: log: vc.phase start phase=solve
// CHECK-NEXT: log: vc.phase end phase=solve time=
// CHECK-NEXT: log: vc.phase start phase=replay
// CHECK-NEXT: log: vc.phase end phase=replay time=
// CHECK-NOT: log:
// QUIET-NOT: log:
// QUIET-NOT: phase
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
