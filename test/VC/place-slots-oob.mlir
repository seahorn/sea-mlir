// The placements differ for a program whose result depends on where an
// out-of-bounds access lands: a + 3 is b's cell when b follows a (the
// plain placement packs them), a padding cell of a's slot under
// --place-slots=aligned. Without --seam: unsat with the plain placement,
// sat with slots (the run reads a's padding; it replays). Under --seam the
// answer rests on A-INB, which the access breaks (vc.seam-out-of-object
// names it; the A-INB check of --check-inb fails on it): the plain
// placement's model does not replay (vc.seam-inb-spurious).
// RUN: sea-vc %s --lower --place-slots=none 2>&1 | FileCheck %s --check-prefix=NONE
// NONE: result: unsat
// RUN: sea-vc %s --lower --place-slots=aligned 2>&1 | FileCheck %s --check-prefix=ALIGNED
// ALIGNED: result: sat
// ALIGNED: replay: assert-failed
// ALIGNED: validate: checked={{[0-9]+}} mismatches=0
// RUN: not sea-vc %s --lower --seam --place-slots=none 2>&1 | FileCheck %s --check-prefix=SEAM
// SEAM: [vc.seam-out-of-object]
// SEAM: [vc.seam-inb-spurious]
// RUN: sea-vc %s --lower --seam --place-slots=aligned 2>&1 | FileCheck %s --check-prefix=SEAMSLOTS
// SEAMSLOTS: [vc.seam-out-of-object]
// SEAMSLOTS: result: sat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @oob {
  %m0 = sea.mem.nondet "m" [0] : !m
  %a = sea.global @a size 3 align 1 : !sea.ptr<64>
  %b = sea.global @b size 1 align 1 : !sea.ptr<64>
  %c7 = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %c7 inplace : !m, i8
  %c3 = sea.const 3 : i64
  %q = sea.ptr.add %a, %c3 : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %c7 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
