// sea-seam-export fails closed: a term the format has no node for (bvxor)
// is an error with status 1 and nothing on stdout; so are an unknown lane,
// a program that is not straight-line, and a missing input. A program the VC
// cannot encode is an error with its vc.* id and location: an abstract
// program (sea.malloc, refused up front as sea-vc refuses it) and a placed
// program with an op OpSem does not encode (sea.tracking_on).
// RUN: not sea-seam-export %s 2>%t.err > %t.out
// RUN: FileCheck %s --check-prefix=XOR < %t.err
// RUN: count 0 < %t.out
// RUN: not sea-seam-export %S/Inputs/abstract.mlir 2>%t.err2 > %t.out2
// RUN: FileCheck %s --check-prefix=ABS < %t.err2
// RUN: count 0 < %t.out2
// RUN: not sea-seam-export %S/Inputs/tracking.mlir 2>%t.err3 > %t.out3
// RUN: FileCheck %s --check-prefix=OP < %t.err3
// RUN: count 0 < %t.out3
// RUN: not sea-seam-export %s --lane=nope 2>&1 | FileCheck %s --check-prefix=LANE
// RUN: not sea-seam-export %S/Inputs/cfg.mlir 2>&1 | FileCheck %s --check-prefix=CFG
// RUN: not sea-seam-export 2>&1 | FileCheck %s --check-prefix=NOINPUT
// RUN: not sea-seam-export %s --rewrite --lane=raw 2>&1 | FileCheck %s --check-prefix=BOTH
// XOR: Error: unsupported expression: bvxor(nd.0_0, nd.1_0) [validation.export]
// ABS: Error: the program uses abstract pointers; place it first {{.*}} at loc({{.*}}) [vc.abstract-program]
// OP: Error: sea.tracking_on is an op the VC does not encode at loc("on") [vc.unsupported-op]
// LANE: unknown lane 'nope' (lanes: memory-rules0, {{.*}}) [validation.export]
// CFG: single block without arguments required [validation.export]
// NOINPUT: no input program
// BOTH: --rewrite is --lane=seam-wide-inb0-simplify0-layout0: not with --lane
sea.program @main {
  %a = sea.nondet "a" [0] : i8 loc("a")
  %b = sea.nondet "b" [1] : i8 loc("b")
  %x = sea.xor %a, %b : i8 loc("x")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
