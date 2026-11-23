// RUN: sea-vc %s --vc-canonical-sums --smt2 2>/dev/null | tr '\n' ' ' | tr -s ' ' | FileCheck %s --implicit-check-not=bvsub --implicit-check-not='((_ extract 7 0) nd.'

// Item 4 of the form (docs/design/CanonicalSums.md): sums stay at full
// width, an extract stays on top. The low byte of the 16-bit X - Y is an
// extract of the canonical 16-bit sum (X + ~Y) + 1, a leaf of the 8-bit sum
// with z; no 8-bit copy of the difference is made.
// CHECK: (bvadd nd.2_0 ((_ extract 7 0) (bvadd nd.0_0 (bvnot nd.1_0) #x0001)) #x03)
sea.program @p {
  %x = sea.nondet "f" [0] : i16
  %y = sea.nondet "f" [1] : i16
  %z = sea.nondet "f" [2] : i8
  %c3 = sea.const 3 : i8
  %d = sea.sub %x, %y : i16
  %lo = sea.trunc %d : i16 to i8
  %s = sea.add %lo, %z : i8
  %s3 = sea.add %s, %c3 : i8
  %c9 = sea.const 9 : i8
  %ok = sea.icmp ult, %s3, %c9 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
