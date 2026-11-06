// sea-seam-export (docs/SeamExport.md), every lane: a pointer stored to
// memory, loaded back and used as the address of a later load. Pins the
// structure (Inputs/show.py checks the document and prints a line per
// record): what each lane does to the loaded pointer and the read.
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
// ALL: header: schema=2 lane=[[LANE]] {{.*}} ptr_mode=opaque address_bits=8
// ALL: root %p sea.addr.from_bits ptr: fromBits(nd.0_0)
// SEAMENC: root %written sea.store memory: store(constMem(0), obj.slot, toBits(fromBits(nd.0_0)))
// NOSEAM: root %written sea.store memory: store(constMem(0), fromInt(2), toBits(fromBits(nd.0_0)))
// The unrewritten read of the stored pointer.
// RAW: root %q sea.load ptr: fromBits(select(store(constMem(0), obj.slot, toBits(fromBits(nd.0_0))), obj.slot))
// RAW: root %read sea.load bv: select(store(constMem(0), fromBits(nd.0_0), 42), fromBits(select(
// NOSEAM: root %q sea.load ptr: fromBits(select(
// Memory rules without simplification: the read resolved, the pointer not.
// REW: root %q sea.load ptr: fromBits(toBits(fromBits(nd.0_0)))
// REW: root %read sea.load bv: 42
// With simplification: ptr.load-resolve.
// SEAM: root %q sea.load ptr: fromBits(nd.0_0)
// SEAM: root %read sea.load bv: 42
// SEAMENC: side: eq(addr(obj.slot), 2)
// SEAMENC: source fixed=2: obj.slot
// NOSEAM: source fixed=2: fromInt(2)
// ALL: probe loc("q") bytes=1:
// ALL: probe loc("read") bytes=1:
// SEAM-SAME: @ fromBits(nd.0_0)
// ALL: erased %p: nd.0_0
// RAW: erased %q: select(store(constMem(0), obj.slot, nd.0_0), obj.slot)
// SEAM: erased %q: nd.0_0
// SEAM: erased %read: 42
// NOSEAM: erased %q: select(store(constMem(0), 2, nd.0_0), 2)
// SEAMENC: erased-side: eq(obj.slot, 2)
sea.program @main {
  %raw = sea.nondet "p" [0] : i8 loc("raw")
  %p = sea.addr.from_bits %raw : i8 to !sea.addr<8> loc("p")
  %slot = sea.segment "slot" base 2 : i8 size 1 : i8 align 1 kind "stack" : !sea.addr<8> loc("slot")
  %zero = sea.const 0 : i8 loc("zero")
  %value = sea.const 42 : i8 loc("value")
  %mem = sea.mem.const %zero : !sea.mem<!sea.addr<8>, i8> loc("mem")
  %written = sea.store %mem[%slot], %p inplace : !sea.mem<!sea.addr<8>, i8>, !sea.addr<8> loc("written")
  %q = sea.load %written[%slot] : !sea.mem<!sea.addr<8>, i8> -> !sea.addr<8> loc("q")
  %data = sea.mem.const %zero : !sea.mem<!sea.addr<8>, i8> loc("data")
  %filled = sea.store %data[%p], %value inplace : !sea.mem<!sea.addr<8>, i8>, i8 loc("filled")
  %read = sea.load %filled[%q] : !sea.mem<!sea.addr<8>, i8> -> i8 loc("read")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
