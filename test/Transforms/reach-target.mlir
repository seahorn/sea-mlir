// RUN: sea-opt --sea-reach-target="id=t.c:5:1" %s | FileCheck %s
// RUN: sea-opt --sea-reach-target="id=t.c:9:1" %s | FileCheck %s --check-prefix=ASSERT
// RUN: sea-opt --sea-reach-target="id=t.c:6:1 kind=blocks" %s | FileCheck %s --check-prefix=BLOCK
// RUN: not sea-opt --sea-reach-target="id=t.c:99:1" %s 2>&1 | FileCheck %s --check-prefix=NOID
// RUN: not sea-opt --sea-reach-target="id=t.c:5:1 kind=ops" %s 2>&1 | FileCheck %s --check-prefix=KIND
// sea-reach-target: R_T, the program with every assert dropped and an
// assert(false) at the top of the target (sea-vc --reach ids). T's block is
// split right before T and branches to `assert false; halt`; T and the rest
// of its block are gone, other halts become sea.unreachable.

// A target in the middle of a block: the ops before it stay, the branch to
// the new block replaces it; the else path's halt is now unreachable.
// CHECK-LABEL: sea.program @main
// CHECK:         sea.assume
// CHECK-NEXT:    sea.icmp ult
// CHECK-NEXT:    sea.br ^[[ERR:bb[0-9]+]]
// CHECK-NOT:     sea.assert
// CHECK:         sea.unreachable
// CHECK-NOT:     sea.assert
// CHECK:       ^[[ERR]]:
// CHECK-NEXT:    %[[F:.*]] = sea.const false
// CHECK-NEXT:    sea.assert %[[F]] {sea.reach_target = "t.c:5:1"}
// CHECK-NEXT:    sea.halt
// CHECK-NOT:     sea.halt

// The original assert as the target: its block ends in the branch.
// ASSERT:       ^{{bb[0-9]+}}:
// ASSERT:         sea.const false
// ASSERT-NEXT:    sea.br ^[[ERR:bb[0-9]+]]
// ASSERT-NOT:     sea.unreachable
// ASSERT:       ^[[ERR]]:
// ASSERT-NEXT:    sea.const false
// ASSERT-NEXT:    sea.assert %{{.*}} {sea.reach_target = "t.c:9:1"}
// ASSERT-NEXT:    sea.halt

// A block target: the block itself is the branch.
// BLOCK:       ^bb2:
// BLOCK-NEXT:    sea.br ^[[ERR:bb[0-9]+]]
// BLOCK:       ^[[ERR]]:
// BLOCK:         sea.assert %{{.*}} {sea.reach_target = "t.c:6:1"}

// NOID: no checks target with id 't.c:99:1' {{.*}}[reach-target.id]
// KIND: kind is checks or blocks, not 'ops' {{.*}}[reach-target.kind]

sea.program @main {
  %x = sea.nondet "x" [0] : i8 loc("t.c":2:1)
  %c5 = sea.const 5 : i8
  %c3 = sea.const 3 : i8
  %big = sea.icmp ugt, %x, %c5 : i8 loc("t.c":2:2)
  sea.cond_br %big, ^then, ^else loc("t.c":2:3)
^then:
  sea.assume %big loc("t.c":3:1)
  %small = sea.icmp ult, %x, %c3 : i8 loc("t.c":4:1)
  sea.assume %small loc("t.c":5:1)
  sea.br ^join loc("t.c":5:2)
^else:
  %e = sea.icmp ne, %x, %c3 : i8 loc("t.c":6:1)
  sea.assume %e loc("t.c":7:1)
  sea.br ^join loc("t.c":7:2)
^join:
  %f = sea.const 0 : i1 loc("t.c":8:1)
  sea.assert %f loc("t.c":9:1)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
