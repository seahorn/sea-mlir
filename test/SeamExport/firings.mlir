// sea-seam-export --firings: the rule firings of the rewriter as witnesses
// (`premise -> lhs = rhs`, the statement sea-vc --seam-check checks). A
// pointer p is stored in object a, a byte in b at a symbolic offset k; the
// pointer is reloaded from a and read through. Under A-INB the store to b is
// skipped (row.store-skip by cmp.diff-base: different objects; premise: the
// layout and k in b's bounds), the reload hits, the reloaded pointer
// resolves (ptr.load-resolve), and the read through it meets the two stores
// as alias markers (unitOverlap). Without A-INB the skip does not fire: the
// reload is an alias-marker ite, resolved by ptr.load-resolve-ite.
// RUN: sea-seam-export %s --lane=seam-wide --firings | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=CHECK,INB
// RUN: sea-seam-export %s --lane=seam --firings | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=CHECK,INB
// RUN: sea-seam-export %s --lane=seam-wide-inb0 --firings | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=CHECK,NOINB
// The raw lanes have no rules, hence no firings: refused.
// RUN: not sea-seam-export %s --lane=raw --firings 2>&1 | FileCheck %s --check-prefix=NORULES
// Without --firings: no firing keys.
// RUN: sea-seam-export %s --lane=seam | FileCheck %s --check-prefix=OFF
// INB: header: schema=2 lane={{seam|seam-wide}} sea_vc='sea-vc --seam{{( --seam-wide)?}}' ainb=true
// NOINB: header: schema=2 lane=seam-wide-inb0 sea_vc='sea-vc --seam --seam-wide --seam-inb=0' ainb=false
// INB: firing 0 rules=row.store-skip,cmp.diff-base premises=A-INB op=load units=1
// INB-NEXT: lhs: select(store(store(constMem(0), obj.a, toBits(fromBits(nd.0_0))), index(obj.b, nd.1_0*1, c=0, K=0, plain), 5), obj.a)
// INB-NEXT: rhs: select(store(constMem(0), obj.a, toBits(fromBits(nd.0_0))), obj.a)
// INB-NEXT: premise: and(eq(addr(obj.a), 16), eq(addr(obj.b), 32), true, ule(nd.1_0, 3))
// NOINB-NOT: row.store-skip
// CHECK: rules=row.store-hit,cmp.refl premises=none op=load units=1
// CHECK-NEXT: lhs: select(store(constMem(0), obj.a, toBits(fromBits(nd.0_0))), obj.a)
// CHECK-NEXT: rhs: toBits(fromBits(nd.0_0))
// CHECK-NEXT: premise: true
// INB: rules=ptr.load-resolve premises=none op=addr
// INB-NEXT: lhs: fromBits(toBits(fromBits(nd.0_0)))
// INB-NEXT: rhs: fromBits(nd.0_0)
// INB-NEXT: premise: true
// INB: rules=row.store premises=none op=load units=1
// INB-NEXT: lhs: select(
// INB-NEXT: rhs: ite(unitOverlap(index(obj.b, nd.1_0*1, c=0, K=0, plain), fromBits(nd.0_0)), 5, select(
// Without A-INB the reloaded pointer is an ite over the store to b, which
// the resolution pulls through.
// NOINB: rules=ptr.load-resolve-ite premises=none op=addr
// NOINB-NEXT: lhs: fromBits(ite(unitOverlap(obj.a, index(obj.b, nd.1_0*1, c=0, K=0, plain)), 5, toBits(fromBits(nd.0_0))))
// NOINB-NEXT: rhs: ite(unitOverlap(obj.a, index(obj.b, nd.1_0*1, c=0, K=0, plain)), fromBits(5), fromBits(nd.0_0))
// CHECK: firings: {{[1-9][0-9]*}} unexpressible=0
// NORULES: --firings needs a lane with rules (lane raw has none) [validation.export]
// OFF-NOT: "firings
sea.program @main {
  %zero = sea.const 0 : i8 loc("zero")
  %m0 = sea.mem.const %zero : !sea.mem<!sea.addr<8>, i8> loc("m0")
  %a = sea.segment "a" base 16 : i8 size 4 : i8 align 1 kind "stack" : !sea.addr<8> loc("a")
  %b = sea.segment "b" base 32 : i8 size 4 : i8 align 1 kind "stack" : !sea.addr<8> loc("b")
  %raw = sea.nondet "p" [0] : i8 loc("raw")
  %p = sea.addr.from_bits %raw : i8 to !sea.addr<8> loc("p")
  %m1 = sea.store %m0[%a], %p inplace : !sea.mem<!sea.addr<8>, i8>, !sea.addr<8> loc("m1")
  %k = sea.nondet "k" [1] : i8 loc("k")
  %bk = sea.addr.add %b, %k : !sea.addr<8>, i8 loc("bk")
  %five = sea.const 5 : i8 loc("five")
  %m2 = sea.store %m1[%bk], %five inplace : !sea.mem<!sea.addr<8>, i8>, i8 loc("m2")
  %q = sea.load %m2[%a] : !sea.mem<!sea.addr<8>, i8> -> !sea.addr<8> loc("q")
  %x = sea.load %m2[%q] : !sea.mem<!sea.addr<8>, i8> -> i8 loc("x")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
