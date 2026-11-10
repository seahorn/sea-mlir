// The cond= field of fuzz.blocked, per operation: what is shown, and where
// the walk stops (`?`). Each program's seed blocks at its one assume.
// RUN: rm -rf %t && mkdir -p %t
// RUN: printf '0=0\n1=0\n3=0\n4=0\n' > %t/zero.txt
// RUN: printf '0=1\n1=2\n' > %t/ne.txt
// RUN: sea-run %s --program=ptrs --fuzz=1 --corpus=%t/zero.txt | FileCheck %s --check-prefix=PTRS
// RUN: sea-run %s --program=funcs --fuzz=1 --corpus=%t/zero.txt | FileCheck %s --check-prefix=FUNCS
// RUN: sea-run %s --program=stored --fuzz=1 | FileCheck %s --check-prefix=STORED
// RUN: sea-run %s --program=casts --fuzz=1 --corpus=%t/zero.txt | FileCheck %s --check-prefix=CASTS
// RUN: sea-run %s --program=depth --fuzz=1 --corpus=%t/zero.txt | FileCheck %s --check-prefix=DEPTH
// RUN: sea-run %s --program=nodes --fuzz=1 --corpus=%t/zero.txt | FileCheck %s --check-prefix=NODES
// RUN: sea-run %s --program=merge --fuzz=1 --corpus=%t/ne.txt | FileCheck %s --check-prefix=MERGE

// Attributes that carry meaning are shown: two ptr.index ops that differ
// only in their strides render differently; globals by name.
// PTRS: fuzz.blocked: 1 op=sea.assume at={{.*}} sites=0 cond=(eq (ptr.index (global @a) s0 1 strides=4,4 offset=0) (ptr.index (global @a) s0 1 strides=4,1 offset=0))
sea.program @ptrs {
  %g = sea.global @"a" size 4 align 4 : !sea.ptr<32>
  %x = sea.nondet "nd" [0] : i8
  %one = sea.const 1 : i8
  %pi = sea.ptr.index %g [%x, %one] strides [4, 4] offset 0 : !sea.ptr<32>, i8, i8
  %pj = sea.ptr.index %g [%x, %one] strides [4, 1] offset 0 : !sea.ptr<32>, i8, i8
  %q = sea.ptr.cmp eq, %pi, %pj : !sea.ptr<32>
  sea.assume %q
  sea.halt
}

// FUNCS: fuzz.blocked: 1 op=sea.assume at={{.*}} sites=- cond=(eq (func_addr @f) (global @b))
sea.program @funcs {
  %f = sea.func_addr @f : !sea.ptr<64>
  %g = sea.global @"b" size 4 align 4 : !sea.ptr<64>
  %q = sea.ptr.cmp eq, %f, %g : !sea.ptr<64>
  sea.assume %q
  sea.halt
}

// A load from a mem.nondet that an update uses (here in place, before the
// load): not the input cells, so `?`.
// STORED: fuzz.blocked: 1 op=sea.assume at={{.*}} sites=- cond=(eq ? 0)
sea.program @stored {
  %m = sea.mem.nondet "heap" [2] : !sea.mem<!sea.addr<64>, i8>
  %c16 = sea.const 16 : i64
  %a16 = sea.addr.from_int %c16 : i64 to !sea.addr<64>
  %c5 = sea.const 5 : i8
  %m1 = sea.store %m[%a16], %c5 inplace : !sea.mem<!sea.addr<64>, i8>, i8
  %v = sea.load %m[%a16] : !sea.mem<!sea.addr<64>, i8> -> i8
  %z0 = sea.const 0 : i8
  %iz = sea.icmp eq, %v, %z0 : i8
  sea.assume %iz
  sea.halt
}

// CASTS: fuzz.blocked: 1 op=sea.assume at={{.*}} sites=0,1,3,4 cond=(and (slt (trunc:4 (sext:32 (select s4 (select s3 s0 s1) s0))) 7) (ugt (extract:8 (sext:32 (select s4 (select s3 s0 s1) s0)) 3) 200))
sea.program @casts {
  %x = sea.nondet "nd" [0] : i8
  %y = sea.nondet "nd" [1] : i8
  %b = sea.nondet "nd" [3] : i1
  %b2 = sea.nondet "nd" [4] : i1
  %s1 = sea.select %b, %x, %y : i8
  %s2 = sea.select %b2, %s1, %x : i8
  %w = sea.sext %s2 : i8 to i32
  %t = sea.trunc %w : i32 to i4
  %e = sea.extract %w[3] : i32 -> i8
  %c7 = sea.const 7 : i4
  %q = sea.icmp slt, %t, %c7 : i4
  %c200 = sea.const 200 : i8
  %q2 = sea.icmp ugt, %e, %c200 : i8
  %r = sea.and %q, %q2 : i1
  sea.assume %r
  sea.halt
}

// Depth: ops more than 6 levels below the condition are `?` (constants
// are shown at any depth).
// DEPTH: fuzz.blocked: 1 op=sea.assume at={{.*}} sites=- cond=(eq (add (add (add (add (add ? 1) 1) 1) 1) 1) 1)
sea.program @depth {
  %x = sea.nondet "nd" [0] : i8
  %c = sea.const 1 : i8
  %a1 = sea.add %x, %c : i8
  %a2 = sea.add %a1, %c : i8
  %a3 = sea.add %a2, %c : i8
  %a4 = sea.add %a3, %c : i8
  %a5 = sea.add %a4, %c : i8
  %a6 = sea.add %a5, %c : i8
  %a7 = sea.add %a6, %c : i8
  %q = sea.icmp eq, %a7, %c : i8
  sea.assume %q
  sea.halt
}

// Size: nodes past the 40th are `?` (the shared subterms count each time).
// NODES: fuzz.blocked: 1 op=sea.assume at={{.*}} sites=0,1 cond=(eq (add (add (add (add (add s0 s1) (add s0 s1)) (add (add s0 s1) (add s0 s1))) (add (add (add s0 s1) (add s0 s1)) (add (add s0 s1) (add s0 s1)))) (add (add (add (add s0 s1) (add ? ?)) ?) ?)) ?)
sea.program @nodes {
  %x = sea.nondet "nd" [0] : i8
  %y = sea.nondet "nd" [1] : i8
  %c = sea.const 1 : i8
  %b1 = sea.add %x, %y : i8
  %b2 = sea.add %b1, %b1 : i8
  %b3 = sea.add %b2, %b2 : i8
  %b4 = sea.add %b3, %b3 : i8
  %b5 = sea.add %b4, %b4 : i8
  %q = sea.icmp eq, %b5, %c : i8
  sea.assume %q
  sea.halt
}

// A cond_br into one block with two different operands: a merged value.
// MERGE: fuzz.blocked: 1 op=sea.assume at={{.*}} sites=- cond=(eq ? 7)
sea.program @merge {
  %x = sea.nondet "nd" [0] : i8
  %y = sea.nondet "nd" [1] : i8
  %c = sea.icmp eq, %x, %y : i8
  sea.cond_br %c, ^m(%x : i8), ^m(%y : i8)
^m(%p: i8):
  %c7 = sea.const 7 : i8
  %q = sea.icmp eq, %p, %c7 : i8
  sea.assume %q
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
