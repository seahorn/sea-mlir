// The boundary's byte expansion of a width term at an ite address pushes
// the byte offset k into the ite leaves, ite(c, A + 24, A) + k ~> ite(c, A + 24 + k, A + k)
// (op::ptr::byteAddress, exact): a compare of a leaf with another byte
// address folds by rewriting, where a bvadd over the ite left it to the
// solver's adders (the @mixed W-ITE check obligation of wide-ite-leaves.mlir
// dumped from sea-vc, z3 5.3.3: 20.8 s standalone -> 0.07 s with the offsets
// in the leaves; the seam-check run of @mixed: 133-197 s with 1 unknown ->
// ~15 s, 0 unknown).
// @boundary: an 8-byte read of the initial memory at ite(c, A + 24, A) stays
// a width term to the boundary; its byte expansion pushes bytes 1..7
// (wide.expanded-ite-offset); --seam-offset-leaves=0 keeps a + k over the
// ite (no counter). The rewriter's own byte addresses (the byte fallback,
// wide.split-read) are pushed by ptrindex.pull-ite, not here.
// @boundary_mut: a wrong expectation, sat with a replay.
// RUN: sea-vc %s --lower --program=boundary --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=BND,UNSAT
// RUN: sea-vc %s --lower --program=boundary --seam --seam-wide --seam-stats --seam-offset-leaves=0 2>&1 | FileCheck %s --check-prefixes=OFF,UNSAT
// RUN: sea-vc %s --lower --program=boundary_mut --seam --seam-wide 2>&1 | FileCheck %s --check-prefix=MUT
// RUN: sea-vc %s --lower --program=boundary --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: %if yices %{ sea-vc %s --lower --program=boundary --seam --seam-wide --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: not sea-vc %s --lower --program=boundary --seam-offset-leaves=4 2>&1 | FileCheck %s --check-prefix=NOSEAM
// BND: seam.rule.wide.expanded-ite-offset: {{[1-9]}}
// OFF-NOT: ite-offset
// UNSAT: result: unsat
// MUT: result: sat
// MUT: replay: assert-failed
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// NOSEAM: --seam-offset-leaves{{.*}}need{{.*}}--seam
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @boundary {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c24 = sea.const 24 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %A24 = sea.ptr.add %A, %c24 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A24, %A : !sea.ptr<64>
  %x = sea.load %m0[%q] : !m -> i64
  %y = sea.load %m0[%A] : !m -> i64
  %z = sea.load %m0[%A24] : !m -> i64
  %e = sea.select %c, %z, %y : i64
  %ok = sea.icmp eq, %x, %e : i64
  sea.assert %ok
  sea.halt
}
sea.program @boundary_mut {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c24 = sea.const 24 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %A24 = sea.ptr.add %A, %c24 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A24, %A : !sea.ptr<64>
  %x = sea.load %m0[%q] : !m -> i64
  %y = sea.load %m0[%A] : !m -> i64
  %z = sea.load %m0[%A24] : !m -> i64
  %e = sea.select %c, %y, %z : i64
  %ok = sea.icmp eq, %x, %e : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
