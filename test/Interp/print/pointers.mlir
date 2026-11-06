// RUN: sea-run %s --input=0=1 | FileCheck %s

// Pointer tokens at the abstract level (docs/design/Print.md, notation A):
// object + offset, `end` one past the end, `oob` outside, `freed`, null,
// functions; the object table line comes before the first use.
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %c = sea.nondet "nondet_bool" [0] : i1
  %c4 = sea.const 4 : i64
  %c8 = sea.const 8 : i64
  %c16 = sea.const 16 : i64
  %c20 = sea.const 20 : i64
  %cm4 = sea.const -4 : i64
  %s = sea.alloca %c16 align 8 [1] label "buf" : i64 -> !sea.ptr<64> loc("s"("t.c":14:3))
  %h = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64> loc("h")
  %g = sea.global @gv size 4 align 4 : !sea.ptr<64> loc("g")
  %f = sea.func_addr @foo : !sea.ptr<64> loc("f")
  %null = sea.ptr.null : !sea.ptr<64> loc("null")
  %n8 = sea.ptr.add %null, %c8 : !sea.ptr<64>, i64 loc("n8")
  %end = sea.ptr.add %s, %c16 : !sea.ptr<64>, i64 loc("end")
  %oob = sea.ptr.add %s, %c20 : !sea.ptr<64>, i64 loc("oob")
  %neg = sea.ptr.add %s, %cm4 : !sea.ptr<64>, i64 loc("neg")
  %g4 = sea.ptr.add %g, %c4 : !sea.ptr<64>, i64 loc("g4")
  %sel = sea.select %c, %h, %g : !sea.ptr<64> loc("sel")
  // CHECK: print.object: stack.1 kind=stack size=16 label=buf at=t.c:14:3
  // CHECK-NEXT: print: s = &stack.1[0]
  sea.print "s" = "s" : !sea.ptr<64>
  // CHECK-NEXT: print: end = &stack.1[16] end
  sea.print "end" = "end" : !sea.ptr<64>
  // CHECK-NEXT: print: oob = &stack.1[20] oob
  sea.print "oob" = "oob" : !sea.ptr<64>
  // CHECK-NEXT: print: neg = &stack.1[-4] oob
  sea.print "neg" = "neg" : !sea.ptr<64>
  // CHECK-NEXT: print: null = null
  sea.print "null" = "null" : !sea.ptr<64>
  // CHECK-NEXT: print: n8 = &null[8]
  sea.print "n8" = "n8" : !sea.ptr<64>
  // CHECK-NEXT: print.object: func.foo kind=func size=0
  // CHECK-NEXT: print: f = &func.foo
  sea.print "f" = "f" : !sea.ptr<64>
  // CHECK-NEXT: print.object: global.gv kind=global size=4
  // CHECK-NEXT: print: g4 = &global.gv[4] end
  sea.print "g4" = "g4" : !sea.ptr<64>
  // CHECK-NEXT: print.object: heap.2 kind=heap size=8
  // CHECK-NEXT: print: sel = &heap.2[0]
  sea.print "sel" = "sel" : !sea.ptr<64>
  sea.br ^next(%h : !sea.ptr<64>)
^next(%arg : !sea.ptr<64> loc("arg")):
  sea.free %arg : !sea.ptr<64>
  // CHECK-NEXT: print: arg = &heap.2[0] freed
  sea.print "arg" = "arg" : !sea.ptr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
