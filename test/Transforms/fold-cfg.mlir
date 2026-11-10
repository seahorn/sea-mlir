// The sea canonicalizers of sea.assume, sea.assert, sea.br and sea.load
// (sea-opt --canonicalize; the lowering's clean-up, sea-vc --lower-opt).
// RUN: sea-opt %s --split-input-file --canonicalize 2>/dev/null | FileCheck %s
// A fold of a constant-false assume or assert is never silent: a warning
// with the op's location; a fold of a block nothing reaches is not made.
// RUN: sea-opt %s --split-input-file --canonicalize --log=warn.fold.assume-false:2 --log=warn.fold.assert-false:2 2>&1 >/dev/null | FileCheck %s --check-prefix=WARN
// WARN-DAG: at loc("f.c":10:3) [fold.assume-false]
// WARN-DAG: at loc("f.c":20:3) [fold.assert-false]
// WARN-NOT: f.c":11:
// WARN-NOT: f.c":21:

// assume(1) is erased; an assume of an unknown condition stays.
// CHECK-LABEL: sea.program @assume_true
// CHECK: %[[X:.*]] = sea.nondet "x"
// CHECK-NOT: sea.assume %{{.*}}const
// CHECK: sea.assume %[[X]]
// CHECK-NEXT: sea.halt
sea.program @assume_true {
  %t = sea.const true
  sea.assume %t
  %x = sea.nondet "x" [0] : i1
  sea.assume %x
  sea.halt
}

// -----
// assume(0): the rest of the block, and the blocks only it reached, are
// gone; the block ends in sea.unreachable. Ops before the assume stay.
// CHECK-LABEL: sea.program @assume_false
// CHECK: sea.nondet "before"
// CHECK-NEXT: sea.unreachable
// CHECK-NOT: sea.nondet "after"
// CHECK-NOT: sea.assert
// CHECK-NOT: sea.halt
// CHECK: }
sea.program @assume_false {
  %y = sea.nondet "before" [0] : i8
  %t = sea.const true
  %g = sea.xor %t, %t : i1
  sea.assume %g loc("f.c":10:3)
  %z = sea.nondet "after" [1] : i1
  sea.br ^err
^err:
  sea.assert %z
  sea.halt
}

// -----
// A block no execution enters is erased, not folded (no warning for it).
// CHECK-LABEL: sea.program @assume_false_dead
// CHECK-NOT: sea.unreachable
// CHECK: sea.halt
sea.program @assume_false_dead {
  sea.halt
^dead:
  %f = sea.const false
  sea.assume %f loc("f.c":11:3)
  sea.halt
}

// -----
// assert(1) is erased.
// CHECK-LABEL: sea.program @assert_true
// CHECK-NOT: sea.assert
// CHECK: sea.halt
sea.program @assert_true {
  %t = sea.const true
  sea.assert %t
  sea.halt
}

// -----
// assert(0): the assert stays and the block ends right after it, in
// sea.halt; the rest (a later assume, a later block) is gone.
// CHECK-LABEL: sea.program @assert_false
// CHECK: %[[F:.*]] = sea.const false
// CHECK: sea.assert %[[F]]
// CHECK-NEXT: sea.halt
// CHECK-NOT: sea.assume
// CHECK-NOT: sea.nondet "after"
// CHECK: }
sea.program @assert_false {
  %f = sea.const false
  sea.assert %f loc("f.c":20:3)
  %z = sea.nondet "after" [0] : i1
  sea.assume %z
  sea.br ^next
^next:
  sea.halt
}

// -----
// SeaHorn's `assert false; halt` is already the end of its block: kept,
// no warning; prints before the halt stay.
// CHECK-LABEL: sea.program @assert_false_halt
// CHECK: sea.assert
// CHECK-NEXT: sea.print
// CHECK-NEXT: sea.halt
sea.program @assert_false_halt {
  %f = sea.const false
  sea.assert %f loc("f.c":21:3)
  sea.print "at end"
  sea.halt
}

// -----
// A chain of single-predecessor blocks is one block; block arguments are
// replaced by the operands.
// CHECK-LABEL: sea.program @merge_chain
// CHECK: %[[X:.*]] = sea.nondet "x"
// CHECK-NOT: sea.br
// CHECK-NOT: ^bb
// CHECK: sea.assert %[[X]]
// CHECK-NEXT: sea.halt
sea.program @merge_chain {
  %x = sea.nondet "x" [0] : i1
  sea.br ^a(%x : i1)
^a(%p: i1):
  sea.br ^b
^b:
  sea.assert %p
  sea.halt
}

// -----
// Not merged: a join (two predecessors), and a branch with a source
// position (the reach site a replay names).
// CHECK-LABEL: sea.program @no_merge
// CHECK: sea.cond_br
// CHECK: sea.br ^[[J:bb[0-9]+]]
// CHECK: sea.br ^[[J]]
// CHECK: ^[[J]]:
// CHECK-NEXT: sea.br ^[[E:bb[0-9]+]]
// CHECK: ^[[E]]:
// CHECK-NEXT: sea.assert
sea.program @no_merge {
  %c = sea.nondet "c" [0] : i1
  sea.cond_br %c, ^l, ^r
^l:
  sea.br ^j
^r:
  sea.br ^j
^j:
  sea.br ^e loc("f.c":30:3)
^e:
  %f = sea.const false
  sea.assert %f
  sea.halt
}

// -----
// load(store(m, a, v), a) -> v: the stored integer, in any layout and
// through a tracked store.
// CHECK-LABEL: sea.program @load_stored
// CHECK: %[[V:.*]] = sea.nondet "v" [1] : i32
// CHECK-NOT: sea.load
// CHECK: sea.icmp eq, %[[V]], %[[V]]
// CHECK: sea.icmp eq, %[[V]], %[[V]]
// CHECK: sea.icmp eq, %[[V]], %[[V]]
sea.program @load_stored {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %v = sea.nondet "v" [1] : i32
  %k = sea.nondet "k" [2] : i64
  %a = sea.addr.from_int %k : i64 to !sea.addr<64>
  %m1 = sea.store %m[%a], %v tracked : !sea.mem<!sea.addr<64>, i8>, i32
  %x = sea.load %m1[%a] : !sea.mem<!sea.addr<64>, i8> -> i32
  %e1 = sea.icmp eq, %x, %v : i32
  %w = sea.mem.nondet "w" [3] : !sea.mem<!sea.addr<64>, i32, aligned>
  %w1 = sea.store %w[%a], %v : !sea.mem<!sea.addr<64>, i32, aligned>, i32
  %y = sea.load %w1[%a] : !sea.mem<!sea.addr<64>, i32, aligned> -> i32
  %e2 = sea.icmp eq, %y, %v : i32
  %q = sea.mem.nondet "q" [4] : !sea.mem<!sea.addr<64>, i32, keyed>
  %q1 = sea.store %q[%a], %v : !sea.mem<!sea.addr<64>, i32, keyed>, i32
  %z = sea.load %q1[%a] : !sea.mem<!sea.addr<64>, i32, keyed> -> i32
  %e3 = sea.icmp eq, %z, %v : i32
  %e12 = sea.and %e1, %e2 : i1
  %e = sea.and %e12, %e3 : i1
  sea.assert %e
  sea.halt
}

// -----
// Not forwarded: another SSA address (even if equal), another type, a
// pointer value, an earlier memory, a memory no store returned.
// CHECK-LABEL: sea.program @load_kept
// CHECK-COUNT-5: sea.load
sea.program @load_kept {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %v = sea.nondet "v" [1] : i32
  %k = sea.nondet "k" [2] : i64
  %a = sea.addr.from_int %k : i64 to !sea.addr<64>
  %b = sea.addr.from_int %k : i64 to !sea.addr<64>
  %m1 = sea.store %m[%a], %v : !sea.mem<!sea.addr<64>, i8>, i32
  %x1 = sea.load %m1[%b] : !sea.mem<!sea.addr<64>, i8> -> i32
  %x2 = sea.load %m1[%a] : !sea.mem<!sea.addr<64>, i8> -> i16
  %m2 = sea.store %m1[%a], %a : !sea.mem<!sea.addr<64>, i8>, !sea.addr<64>
  %x3 = sea.load %m2[%a] : !sea.mem<!sea.addr<64>, i8> -> !sea.addr<64>
  %x4 = sea.load %m[%a] : !sea.mem<!sea.addr<64>, i8> -> i32
  %c4 = sea.const 4 : i64
  %m3 = sea.memhavoc %m2[%a], %c4 [3] : !sea.mem<!sea.addr<64>, i8>
  %x5 = sea.load %m3[%a] : !sea.mem<!sea.addr<64>, i8> -> i32
  sea.print "x1" %x1 : i32
  sea.print "x2" %x2 : i16
  sea.print "x3" %x3 : !sea.addr<64>
  sea.print "x4" %x4 : i32
  sea.print "x5" %x5 : i32
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
