// sea-seam-export, every lane: three stores into one object (one of them
// overwriting) and two loads. The memory rules compress them into a
// store-map (smap: base memory, object, entries newest first) and resolve
// both reads; without them the store chain stays.
// RUN: sea-seam-export %s --address-bits=8 | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,RAW -DLANE=memory-rules0
// RUN: sea-seam-export %s --rewrite | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,REW -DLANE=seam-wide-inb0-simplify0-layout0
// RUN: sea-seam-export %s --lane=raw-wide | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,RAW -DLANE=raw-wide
// RUN: sea-seam-export %s --lane=raw | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,RAW -DLANE=raw
// RUN: sea-seam-export %s --lane=seam-wide-inb0 | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,SEAM -DLANE=seam-wide-inb0
// RUN: sea-seam-export %s --lane=seam-inb0 | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,SEAM -DLANE=seam-inb0
// RUN: sea-seam-export %s --lane=seam-wide | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,SEAM,INB -DLANE=seam-wide
// RUN: sea-seam-export %s --lane=seam | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,SEAM,INB -DLANE=seam
// RUN: sea-seam-export %s --lane=plain | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,NOSEAM,PLAIN -DLANE=plain
// RUN: sea-seam-export %s --lane=simplify | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,NOSEAM,SIMP -DLANE=simplify
// ALL: header: schema=2 lane=[[LANE]]
// SEAMENC: root %s1 sea.addr.add ptr: index(obj.s, c=1, K=0, plain)
// NOSEAM: root %s1 sea.addr.add ptr: index(fromInt(8), c=1, K=0, plain)
// RAW: root %m3 sea.store memory: store(store(store(constMem(0), index(obj.s, c=1, K=0, plain), nd.0_0), index(obj.s, c=2, K=0, plain), 1), index(obj.s, c=1, K=0, plain), 2)
// RAW: root %r1 sea.load bv: select(store(store(store(
// REW: root %m3 sea.store memory: smap(constMem(0), obj.s, 1:2, 2:1, 1:nd.0_0)
// REW: root %r1 sea.load bv: 2
// REW: root %r0 sea.load bv: select(constMem(0), obj.s)
// SEAM: root %m3 sea.store memory: smap(constMem(0), obj.s, 1:2, 2:1, 1:nd.0_0)
// SEAM: root %r1 sea.load bv: 2
// SEAM: root %r0 sea.load bv: select(constMem(0), obj.s)
// NOSEAM: root %r1 sea.load bv: select(store(store(store(
// ALL: probe loc("r1") bytes=1:
// SEAM: probe loc("r0") bytes=1: smap(constMem(0), obj.s, 1:2, 2:1, 1:nd.0_0) @ obj.s
// The boundary expands the store-map into stores; the reads resolved stay.
// RAW: erased %r1: select(store(store(store(constMem(0), add(obj.s, 1), nd.0_0), add(obj.s, 2), 1), add(obj.s, 1), 2), add(obj.s, 1))
// SEAM: erased %r1: 2
// PLAIN: erased %r1: select(store(store(store(constMem(0), add(8, 1), nd.0_0), add(8, 2), 1), add(8, 1), 2), add(8, 1))
// SIMP: erased %r1: select(store(store(store(constMem(0), 9, nd.0_0), 10, 1), 9, 2), 9)
sea.program @main {
  %zero = sea.const 0 : i8 loc("zero")
  %m0 = sea.mem.const %zero : !sea.mem<!sea.addr<8>, i8> loc("m0")
  %s = sea.segment "s" base 8 : i8 size 8 : i8 align 1 kind "stack" : !sea.addr<8> loc("s")
  %x = sea.nondet "x" [0] : i8 loc("x")
  %one = sea.const 1 : i8 loc("one")
  %two = sea.const 2 : i8 loc("two")
  %s1 = sea.addr.add %s, %one : !sea.addr<8>, i8 loc("s1")
  %s2 = sea.addr.add %s, %two : !sea.addr<8>, i8 loc("s2")
  %m1 = sea.store %m0[%s1], %x inplace : !sea.mem<!sea.addr<8>, i8>, i8 loc("m1")
  %m2 = sea.store %m1[%s2], %one inplace : !sea.mem<!sea.addr<8>, i8>, i8 loc("m2")
  %m3 = sea.store %m2[%s1], %two inplace : !sea.mem<!sea.addr<8>, i8>, i8 loc("m3")
  %r1 = sea.load %m3[%s1] : !sea.mem<!sea.addr<8>, i8> -> i8 loc("r1")
  %r0 = sea.load %m3[%s] : !sea.mem<!sea.addr<8>, i8> -> i8 loc("r0")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
