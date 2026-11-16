// sea-seam-export fails closed: each case below is an error with status 1
// and nothing on stdout. A program the VC cannot encode is an error with its
// vc.* id and location: an abstract program (sea.malloc,
// sea.mem.is_modified; refused up front as sea-vc refuses it) and a placed
// program with an op OpSem does not encode (sea.tracking_on). So are an
// unknown lane, a program that is not straight-line, a missing input, and a
// term the exporter refuses itself (a memory at another address width).
// RUN: not sea-seam-export %s 2>%t.err > %t.out
// RUN: FileCheck %s --check-prefix=ABS < %t.err
// RUN: count 0 < %t.out
// RUN: not sea-seam-export %S/Inputs/tracking.mlir 2>%t.err3 > %t.out3
// RUN: FileCheck %s --check-prefix=OP < %t.err3
// RUN: count 0 < %t.out3
// RUN: not sea-seam-export %S/Inputs/addr16.mlir 2>%t.err2 > %t.out2
// RUN: FileCheck %s --check-prefix=WIDTH < %t.err2
// RUN: count 0 < %t.out2
// RUN: not sea-seam-export %s --lane=nope 2>&1 | FileCheck %s --check-prefix=LANE
// RUN: not sea-seam-export %S/Inputs/cfg.mlir 2>&1 | FileCheck %s --check-prefix=CFG
// RUN: not sea-seam-export 2>&1 | FileCheck %s --check-prefix=NOINPUT
// RUN: not sea-seam-export %s --rewrite --lane=raw 2>&1 | FileCheck %s --check-prefix=BOTH
// ABS: Error: the program uses abstract pointers; place it first {{.*}} at loc({{.*}}) [vc.abstract-program]
// OP: Error: sea.tracking_on is an op the VC does not encode at loc("on") [vc.unsupported-op]
// WIDTH: Error: only configured-address-width byte arrays supported [validation.export]
// LANE: unknown lane 'nope' (lanes: memory-rules0, {{.*}}) [validation.export]
// CFG: single block without arguments required [validation.export]
// NOINPUT: no input program
// BOTH: --rewrite is --lane=seam-wide-inb0-simplify0-layout0: not with --lane
!pm = !sea.mem<!sea.ptr<8>, i8>
sea.program @main {
  %z = sea.const 0 : i8 loc("z")
  %m0 = sea.mem.const %z : !pm loc("m0")
  %n = sea.const 8 : i8 loc("n")
  %p = sea.malloc %n align 1 [1] : i8 -> !sea.ptr<8> loc("p")
  %b = sea.mem.is_modified %m0[%p] : !pm loc("b")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
