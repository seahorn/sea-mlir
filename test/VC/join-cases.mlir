// sea-vc --vc-join-cases (experimental): the joins of block arguments
// (values and memories) and of pointer companions are built as cases terms
// and expanded at once to the ite chain, so the query is byte-identical to
// the one without the option; the option adds the cases.* counter lines.
// RUN: sea-vc %s --program=vals --smt2 > %t.vals.off 2> /dev/null
// RUN: sea-vc %s --program=vals --smt2 --vc-join-cases > %t.vals.on 2> %t.vals.err
// RUN: cmp %t.vals.off %t.vals.on
// RUN: FileCheck %s --check-prefix=VALS < %t.vals.err
// RUN: sea-vc %s --program=mem --smt2 > %t.mem.off 2> /dev/null
// RUN: sea-vc %s --program=mem --smt2 --vc-join-cases > %t.mem.on 2> %t.mem.err
// RUN: cmp %t.mem.off %t.mem.on
// RUN: FileCheck %s --check-prefix=MEM < %t.mem.err
// RUN: sea-vc %s --program=comp --lower --seam --ptr-companion=exact --smt2 > %t.comp.off 2> /dev/null
// RUN: sea-vc %s --program=comp --lower --seam --ptr-companion=exact --smt2 --vc-join-cases > %t.comp.on 2> %t.comp.err
// RUN: cmp %t.comp.off %t.comp.on
// RUN: FileCheck %s --check-prefix=COMP < %t.comp.err
// Without --smt2 the counters go to stdout; the verdict is unchanged.
// RUN: sea-vc %s --program=mem --vc-join-cases | FileCheck %s --check-prefix=SOLVE
// Off by default: no counters.
// RUN: sea-vc %s --program=mem | FileCheck %s --check-prefix=OFF
// Only the ite join has an ite chain to build.
// RUN: not sea-vc %s --program=mem --vc-join=eq --vc-join-cases 2>&1 | FileCheck %s --check-prefix=EQ

// Two joins of i8 values: ^j over two edges, ^k over three edges (one of
// which passes the same value as another, for each argument).
// VALS: cases.joins: 3 scalar=3 memory=0 companion=0
// VALS: cases.arity.2: 1 scalar=1
// VALS: cases.arity.3: 2 scalar=2
// VALS: cases.with-dup: 2 scalar=2
// VALS: cases.ite-folded-pairs: 1 scalar=1
// VALS: cases.nonadjacent-redundant: 1 scalar=1
sea.program @vals {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c = sea.nondet "nondet_bool" [1] : i1
  %d = sea.nondet "nondet_bool" [2] : i1
  %c10 = sea.const 10 : i8
  %small = sea.icmp ult, %x, %c10 : i8
  sea.cond_br %small, ^then, ^else
^then:
  %s = sea.add %x, %x : i8
  sea.br ^j(%s : i8)
^else:
  %c1 = sea.const 1 : i8
  sea.br ^j(%c1 : i8)
^j(%y : i8):
  sea.cond_br %c, ^a, ^bc
^bc:
  sea.cond_br %d, ^b, ^cc
^a:
  sea.br ^k(%y, %x : i8, i8)
^b:
  %y2 = sea.add %y, %y : i8
  sea.br ^k(%y2, %x : i8, i8)
^cc:
  sea.br ^k(%y, %c10 : i8, i8)
^k(%u : i8, %v : i8):
  %c40 = sea.const 40 : i8
  %ok = sea.icmp ult, %u, %c40 : i8
  sea.assert %ok
  sea.halt
}

!m8 = !sea.mem<!sea.addr<8>, i8>

// A memory and a value joined over three edges; two edges pass the
// unchanged memory.
// MEM: cases.joins: 2 scalar=1 memory=1 companion=0
// MEM: cases.arity.3: 2 scalar=1 memory=1
// MEM: cases.redundant-values: 1 scalar=0 memory=1
// MEM: cases.adjacent-equal-pairs: 1 scalar=0 memory=1
// MEM: cases.ite-folded-pairs: 0
// MEM: cases.default-not-most-frequent: 1 scalar=0 memory=1
// SOLVE: cases.joins: 2
// SOLVE: result: sat
// OFF-NOT: cases.
// OFF: result: sat
// EQ: --vc-join-cases builds the ite joins: needs --vc-join=ite
sea.program @mem {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c = sea.nondet "nondet_bool" [1] : i1
  %d = sea.nondet "nondet_bool" [2] : i1
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  sea.cond_br %c, ^t, ^e
^e:
  sea.cond_br %d, ^e1, ^e2
^t:
  %m1 = sea.store %m0[%a], %one inplace : !m8, i8
  sea.br ^j(%m1, %one : !m8, i8)
^e1:
  sea.br ^j(%m0, %two : !m8, i8)
^e2:
  sea.br ^j(%m0, %z : !m8, i8)
^j(%m3 : !m8, %w : i8):
  %x = sea.load %m3[%a] : !m8 -> i8
  %ok = sea.icmp eq, %x, %one : i8
  sea.assert %ok
  sea.halt
}

!mp = !sea.mem<!sea.ptr<64>, i8>

// A pointer stored on one branch; the joined memory has a pointer
// companion (--seam --ptr-companion=exact), joined like the memory.
// COMP: cases.joins: 2 scalar=0 memory=1 companion=1
// COMP: cases.arity.2: 2 scalar=0 memory=1 companion=1
sea.program @comp {
  %m0 = sea.mem.nondet "m" [0] : !mp
  %c = sea.nondet "nondet_bool" [1] : i1
  %c8 = sea.const 8 : i64
  %p = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %q = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  sea.cond_br %c, ^t, ^e
^t:
  %m1 = sea.store %m0[%p], %q inplace : !mp, !sea.ptr<64>
  sea.br ^j(%m1 : !mp)
^e:
  %m2 = sea.store %m0[%p], %p inplace : !mp, !sea.ptr<64>
  sea.br ^j(%m2 : !mp)
^j(%m3 : !mp):
  %r = sea.load %m3[%p] : !mp -> !sea.ptr<64>
  %ok = sea.ptr.cmp eq, %r, %q : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
