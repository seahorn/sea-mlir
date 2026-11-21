// cmp.congruence on table slots indexed by a loaded value (reduced from a
// firedancer reachability job). Slots of 32 bytes; prev is loaded from
// slot[i]; -1 is stored at slot[prev] + 20, then slot[i] + 8 is read
// again. The ROW compare of the read over the store, 32i + 8 vs
// 32 zext(prev) + 20, is never true (32 divides the variable part, not
// 12), for every i and prev; ARM gives prev no range, so only
// cmp.congruence decides it. Default: the store is skipped and no
// compare is left. Off: the compare stays in the query. Same verdict.
// RUN: sea-vc %s --lower --seam --widen-memory=4 --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,ON
// RUN: sea-vc %s --lower --seam --widen-memory=4 --seam-stats --seam-congruence=false 2>&1 | FileCheck %s --check-prefixes=CHECK,OFF
// ON-NOT: seam.alias.open
// ON: seam.rule.cmp.congruence: {{[1-9]}}
// ON-NOT: seam.rule.row.store:
// ON: seam.rule.row.store-skip: {{[1-9]}}
// OFF: seam.alias.open.same-object: {{[1-9]}}
// OFF-NOT: cmp.congruence
// OFF: seam.rule.row.store: {{[1-9]}}
// OFF-NOT: seam.rule.row.store-skip
// CHECK: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @mini {
  %m0 = sea.mem.nondet "region" [0] : !m
  %i = sea.nondet "i" [1] : i32
  %ws = sea.global @"ws" size 304 align 16 : !sea.ptr<64>
  %zi = sea.zext %i : i32 to i64
  %pprev = sea.ptr.index %ws [%zi] strides [32] offset 96 inbounds : !sea.ptr<64>, i64
  %prev = sea.load %m0[%pprev] align 16 : !m -> i32
  %zp = sea.zext %prev : i32 to i64
  %pi8 = sea.ptr.index %ws [%zi] strides [32] offset 88 inbounds : !sea.ptr<64>, i64
  %v0 = sea.load %m0[%pi8] align 8 : !m -> i32
  %pn = sea.ptr.index %ws [%zp] strides [32] offset 100 inbounds : !sea.ptr<64>, i64
  %neg1 = sea.const -1 : i32
  %m1 = sea.store %m0[%pn], %neg1 inplace align 4 : !m, i32
  %v1 = sea.load %m1[%pi8] align 8 : !m -> i32
  %ok = sea.icmp eq, %v0, %v1 : i32
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
