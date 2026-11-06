// sea-reduce-cleanup: dead ops (also nondets and memory updates, which
// MLIR does not consider trivially dead), blocks without predecessors,
// unused block arguments; a block whose only predecessor branches to it
// unconditionally is merged into it. Sites of the survivors are kept.
// RUN: sea-opt %s --sea-reduce-cleanup | sea-opt | FileCheck %s
// erase-dead alone (a pattern: one op at a time, the first in order).
// RUN: sea-opt %s --sea-reduce-apply="pattern=erase-dead limit=1" | sea-opt | FileCheck %s --check-prefix=ONE

// CHECK-LABEL: sea.program @main
// CHECK-NEXT:  %[[A:.*]] = sea.nondet "nd" [1] : i8
// CHECK-NEXT:  %[[C:.*]] = sea.const 1 : i8
// CHECK-NEXT:  %[[Z:.*]] = sea.icmp ne, %[[A]], %[[C]]
// CHECK-NEXT:  sea.assert %[[Z]]
// CHECK-NEXT:  sea.halt

// ONE-LABEL: sea.program @main
// ONE-NOT:   sea.mem.nondet "heap"
// ONE:       sea.nondet "nd" [1]
// ONE:       sea.nondet "dead" [2]

sea.program @main {
  %m0 = sea.mem.nondet "heap" [0] : !sea.mem<!sea.addr<8>, i8>
  %a = sea.nondet "nd" [1] : i8
  %u = sea.nondet "dead" [2] : i8
  %c1 = sea.const 1 : i8
  sea.br ^bb1(%a, %u : i8, i8)
^bb1(%x : i8, %y : i8):
  sea.br ^bb3(%x : i8)
^bb2(%w : i8):
  sea.br ^bb3(%w : i8)
^bb3(%v : i8):
  %z = sea.icmp ne, %v, %c1 : i8
  sea.assert %z
  sea.halt
}

// The parts: dead-ops=false keeps the dead nondets; blocks=false keeps the
// CFG (and the nondet a branch still passes).
// RUN: sea-opt %s --sea-reduce-cleanup="dead-ops=false" | sea-opt | FileCheck %s --check-prefix=NODEAD
// RUN: sea-opt %s --sea-reduce-cleanup="blocks=false" | sea-opt | FileCheck %s --check-prefix=NOBLOCKS
// NODEAD:     sea.nondet "dead" [2]
// NODEAD-NOT: ^bb
// NOBLOCKS-NOT: sea.mem.nondet "heap"
// NOBLOCKS:     ^bb2

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
