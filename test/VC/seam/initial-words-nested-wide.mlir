// The initial-words boundary (--seam-words) expands width terms over the
// erased query: a wide read whose address holds another wide read's
// expansion (a 4-byte load indexed by a 4-byte load of the same memory, not
// re-encoded as words: the inner load is the only aligned read) needs the
// inner byte addresses flat, or the outer one has no address sort (it
// crashed in WideExpander::at). The verdict replays; --words-check compares
// the word query with the byte query.
// RUN: sea-vc %s --lower --seam --seam-wide --seam-words=auto 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-wide --seam-words=auto --words-check 2>&1 | FileCheck %s --check-prefix=CHECKW
// RUN: sea-vc %s --lower --seam --seam-wide --seam-words=all 2>&1 | FileCheck %s

// CHECK: result: sat
// CHECK: replay: assert-failed
// CHECK: validate: {{.*}} mismatches=0
// CHECKW: words.check: ok
// CHECKW: result: sat

sea.program @main {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8>
  %g = sea.global @g size 4096 align 16 : !sea.ptr<64>
  %a = sea.load %m[%g] : !sea.mem<!sea.ptr<64>, i8> -> i32
  %z = sea.zext %a : i32 to i64
  %p = sea.ptr.index %g[%z] strides [1] offset 0 : !sea.ptr<64>, i64
  %b = sea.load %m[%p] : !sea.mem<!sea.ptr<64>, i8> -> i32
  %c = sea.const 7 : i32
  %e = sea.icmp ult, %b, %c : i32
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
