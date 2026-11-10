
sea.program @merge_chain {
  %x = sea.nondet "x" [0] : i1
  sea.br ^a(%x : i1)
^a(%p: i1):
  sea.br ^b
^b:
  sea.assert %p
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
