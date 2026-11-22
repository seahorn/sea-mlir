// RUN: not sea-opt %s --sea-erase-mem-ssa -split-input-file 2>&1 | FileCheck %s

// sea-erase-mem-ssa refuses what one linear region cannot express.

// A load of an earlier version.
// CHECK: erase-mem-ssa.non-linear
sea.program @old_version {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%p], %v : !sea.mem<!sea.ptr<64>, i8>, i8
  %x = sea.load %m0[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.halt
}

// -----

// A version consumed twice on one path.
// CHECK: erase-mem-ssa.non-linear
sea.program @twice {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%p], %v : !sea.mem<!sea.ptr<64>, i8>, i8
  %m2 = sea.store %m0[%p], %v : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.halt
}

// -----

// A join without a memory argument whose predecessors leave different
// versions, one of which is read after it.
// CHECK: erase-mem-ssa.non-linear
sea.program @join_no_arg {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %c = sea.nondet "c" [2] : i1
  sea.cond_br %c, ^a, ^j
^a:
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%p], %v : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.br ^j
^j:
  %x = sea.load %m0[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.halt
}

// -----

// Two independent memories.
// CHECK: 2 memories{{.*}}[erase-mem-ssa.regions]
sea.program @two {
  %a = sea.mem.nondet "a" [0] : !sea.mem<!sea.ptr<64>, i8>
  %b = sea.mem.nondet "b" [1] : !sea.mem<!sea.ptr<64>, i8>
  sea.halt
}

// -----

// A memmove between two memories is two regions too.
// CHECK: erase-mem-ssa.regions
sea.program @two_move {
  %a = sea.mem.nondet "a" [0] : !sea.mem<!sea.ptr<64>, i8>
  %b = sea.mem.nondet "b" [1] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  %n = sea.const 4 : i64
  %a1 = sea.memmove %a[%p], %b[%p], %n inplace : !sea.mem<!sea.ptr<64>, i8>
  sea.halt
}

// -----

// A block with two memory arguments: two versions live at once.
// CHECK: two memory arguments{{.*}}[erase-mem-ssa.non-linear]
sea.program @two_args {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %z = sea.const 0 : i8
  %m1 = sea.store %m0[%p], %z : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.br ^b(%m0, %m1 : !sea.mem<!sea.ptr<64>, i8>, !sea.mem<!sea.ptr<64>, i8>)
^b(%a : !sea.mem<!sea.ptr<64>, i8>, %b : !sea.mem<!sea.ptr<64>, i8>):
  %x = sea.load %b[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.halt
}

// -----

// A stale branch operand: an earlier version passed to a block argument,
// even one nothing reads.
// CHECK: sea.br uses a memory version that is not the current one{{.*}}[erase-mem-ssa.non-linear]
sea.program @stale_branch {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %z = sea.const 0 : i8
  %m1 = sea.store %m0[%p], %z : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.br ^b(%m0 : !sea.mem<!sea.ptr<64>, i8>)
^b(%a : !sea.mem<!sea.ptr<64>, i8>):
  sea.halt
}

// -----

// A strong print.mem of an earlier version: seaimp.print.mem shows the
// region at its point only.
// CHECK: sea.print.mem uses a memory version that is not the current one{{.*}}[erase-mem-ssa.non-linear]
sea.program @print_old {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %z = sea.const 0 : i8
  %m1 = sea.store %m0[%p], %z : !sea.mem<!sea.ptr<64>, i8>, i8
  sea.print.mem "m" %m1[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.print.mem "old" %m0[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.halt
}

// -----

// A load of an earlier version in a sea.box.all body.
// CHECK: sea.box.all uses a memory version that is not the current one{{.*}}[erase-mem-ssa.non-linear]
sea.program @box_old {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %z = sea.const 0 : i8
  %n = sea.const 4 : i64
  %m1 = sea.store %m0[%p], %z : !sea.mem<!sea.ptr<64>, i8>, i8
  %all = sea.box.all (%i) = (0) to (%n) {
    %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
    %x = sea.load %m0[%q] : !sea.mem<!sea.ptr<64>, i8> -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// -----

// A sea.mem.const root: a region starts as its variable's nondet initial
// value, and seaimp code cannot set a memory.
// CHECK: sea.mem.const: a memory root other than sea.mem.nondet in the entry block{{.*}}[erase-mem-ssa.unsupported]
sea.program @const_root {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %x = sea.load %m0[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.halt
}

// -----

// A sea.mem.nondet root outside the entry block.
// CHECK: sea.mem.nondet: a memory root other than sea.mem.nondet in the entry block{{.*}}[erase-mem-ssa.unsupported]
sea.program @late_root {
  sea.br ^b
^b:
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %x = sea.load %m0[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.halt
}

// -----

// A second root of the one memory (a whole-memory restart joined to the
// first chain).
// CHECK: sea.mem.nondet: a second root of the memory{{.*}}[erase-mem-ssa.unsupported]
sea.program @second_root {
  %m0 = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
  %c = sea.nondet "c" [1] : i1
  sea.cond_br %c, ^a, ^j(%m0 : !sea.mem<!sea.ptr<64>, i8>)
^a:
  %m1 = sea.mem.nondet "fresh" [2] : !sea.mem<!sea.ptr<64>, i8>
  sea.br ^j(%m1 : !sea.mem<!sea.ptr<64>, i8>)
^j(%mj : !sea.mem<!sea.ptr<64>, i8>):
  sea.halt
}

// -----

// A memory without a root (block arguments of an unreachable loop only).
// CHECK: sea.program: a memory without a root{{.*}}[erase-mem-ssa.unsupported]
sea.program @no_root {
  sea.halt
^loop(%m : !sea.mem<!sea.ptr<64>, i8>):
  sea.br ^loop(%m : !sea.mem<!sea.ptr<64>, i8>)
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
