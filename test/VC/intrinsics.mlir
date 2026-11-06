// RUN: not sea-vc %s 2>&1 | FileCheck %s --check-prefix=RAW
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC

// The VC refuses intrinsics; --lower expands them first.
// RAW: vc.intrinsic
// The assertion "no unsigned overflow" fails for some x, y; replay confirms.
// VC: result: sat
// VC: replay: assert-failed
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %o = sea.uaddo %x, %y : i8
  %t = sea.const 1 : i1
  %ok = sea.xor %o, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
