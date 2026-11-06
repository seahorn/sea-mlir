// PTR_DIFF (docs/design/PtrSort.md 1.6, S1.5): sea.ptr.diff is a PTR_DIFF
// term under --seam, with the diff.* rules; a bvsub without it. Each
// program's verdict is the same with and without SeaM.
// @same: (A + i) - (A + j) is i - j (diff.same-base): unsat.
// @ite: ite(c, A + i, A + j) - A is ite(c, i, j) (diff.ite-same-base): unsat.
// @self: p - p is 0 (diff.self): unsat.
// @other: A - B for two objects is not 0 (diff.erase, then bvsub): the
// assertion A - B == 0 fails, sat.
// RUN: sea-vc %s --program=same --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=same --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,SAME
// RUN: sea-vc %s --program=ite --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=ite --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,ITE
// RUN: sea-vc %s --program=self --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=self --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,SELF
// RUN: sea-vc %s --program=other --lower | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=other --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=SAT,OTHER
// RUN: sea-vc %s --program=other --lower --seam --seam-simplify=0 2>&1 | FileCheck %s --check-prefix=SAT
// SAME: seam.rule.diff.same-base: {{[1-9]}}
// ITE: seam.rule.diff.ite-same-base: {{[1-9]}}
// SELF: seam.rule.diff.self: {{[1-9]}}
// OTHER: seam.rule.diff.erase: {{[1-9]}}
// UNSAT: result: unsat
// SAT: result: sat
// SAT: replay: assert-failed
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @same {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %q = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %d = sea.ptr.diff %p, %q : !sea.ptr<64> -> i64
  %e = sea.sub %i, %j : i64
  %ok = sea.icmp eq, %d, %e : i64
  sea.assert %ok
  sea.halt
}
sea.program @ite {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %c = sea.nondet "c" [4] : i1
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %q = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %s = sea.select %c, %p, %q : !sea.ptr<64>
  %d = sea.ptr.diff %s, %A : !sea.ptr<64> -> i64
  %e = sea.select %c, %i, %j : i64
  %ok = sea.icmp eq, %d, %e : i64
  sea.assert %ok
  sea.halt
}
sea.program @self {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %d = sea.ptr.diff %p, %p : !sea.ptr<64> -> i64
  %z = sea.const 0 : i64
  %ok = sea.icmp eq, %d, %z : i64
  sea.assert %ok
  sea.halt
}
sea.program @other {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %d = sea.ptr.diff %A, %B : !sea.ptr<64> -> i64
  %z = sea.const 0 : i64
  %ok = sea.icmp eq, %d, %z : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
