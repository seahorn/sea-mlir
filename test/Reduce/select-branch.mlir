// select-branch: cond_br -> br to the true successor with its block
// arguments; to the false one when the true one ends in sea.unreachable.
// The block left without predecessors goes with sea-reduce-cleanup.
// RUN: sea-opt %s --sea-reduce-apply="pattern=select-branch limit=1" | sea-opt | FileCheck %s --check-prefix=ONE
// RUN: sea-opt %s --sea-reduce-apply="pattern=select-branch" --sea-reduce-cleanup | sea-opt | FileCheck %s --check-prefix=ALL

// erase-unreachable: the block left without predecessors goes at once.
// RUN: sea-opt %s --sea-reduce-apply="pattern=select-branch limit=1 erase-unreachable=true" | sea-opt | FileCheck %s --check-prefix=ERASE
// ERASE-LABEL: sea.program @main
// ERASE-NOT:   no predecessors

// ONE-LABEL: sea.program @main
// ONE:       %[[A:.*]] = sea.nondet "nd" [0]
// ONE:       %[[ONE:.*]] = sea.const 1
// ONE:       sea.br ^bb1(%[[A]] : i8)
// ONE:     ^bb3(
// ONE:       sea.cond_br %{{.*}}, ^bb4, ^bb5

// The second cond_br takes ^bb5 (^bb4 is unreachable); cleanup merges the
// straight line into one block.
// ALL-LABEL: sea.program @main
// ALL-NOT:   ^bb
// ALL-NOT:   sea.cond_br
// ALL-NOT:   sea.unreachable
// ALL:       sea.assert
// ALL-NEXT:  sea.halt

sea.program @main {
  %a = sea.nondet "nd" [0] : i8
  %c1 = sea.const 1 : i8
  %d = sea.icmp ult, %a, %c1 : i8
  sea.cond_br %d, ^bb1(%a : i8), ^bb2(%c1 : i8)
^bb1(%x : i8):
  sea.br ^bb3(%x : i8)
^bb2(%y : i8):
  sea.br ^bb3(%y : i8)
^bb3(%v : i8):
  %e = sea.icmp eq, %v, %c1 : i8
  sea.cond_br %e, ^bb4, ^bb5
^bb4:
  sea.unreachable
^bb5:
  %z = sea.icmp ne, %v, %a : i8
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
