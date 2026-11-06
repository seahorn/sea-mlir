// Locations of inlined ops (callsite(<callee> at <caller>), nested either
// way) in a failing run: assert-at, assert-op, assert-path entries and
// print at= show the innermost position, then ` inlined-at=` the call
// sites (outermost last), then the name (jsonl: `<field>_inlined` arrays,
// `path_inlined` parallel to `path`). A fused location's metadata (the
// exporter's `llvm @f %bb #i` link) is never a position or a name; a fused
// location without a file position gets its position in this file.
// RUN: sea-vc %s --program=inl --lower | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --program=inl --lower --format=jsonl \
// RUN:   | FileCheck %s --check-prefix=VCJSON
// RUN: sea-run %s --program=inl --input=0=7 --print-locs \
// RUN:   | FileCheck %s --check-prefix=INTERP
// RUN: sea-run %s --program=inl --input=0=7 --print-locs --format=jsonl \
// RUN:   | FileCheck %s --check-prefix=IJSON
// RUN: sea-run %s --program=path --input=0=1 | FileCheck %s --check-prefix=PATH
// RUN: sea-run %s --program=path --input=0=1 --format=jsonl \
// RUN:   | FileCheck %s --check-prefix=PJSON
// RUN: sea-vc %s --program=inl --lower --print-locs > %t.vc.txt
// RUN: sea-vc %s --program=inl --lower --print-locs --format=jsonl > %t.vc.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.vc.txt \
// RUN:   --jsonl %t.vc.jsonl --require replay:at_inlined,op_inlined,op_name \
// RUN:   --require print:at_inlined,at_name
// RUN: sea-run %s --program=inl --input=0=7 --print-locs > %t.run.txt
// RUN: sea-run %s --program=inl --input=0=7 --print-locs --format=jsonl > %t.run.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.run.txt \
// RUN:   --jsonl %t.run.jsonl --require print.object:at_inlined,at_name
// RUN: sea-run %s --program=path --input=0=1 > %t.path.txt
// RUN: sea-run %s --program=path --input=0=1 --format=jsonl > %t.path.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.path.txt \
// RUN:   --jsonl %t.path.jsonl --require status:path,path_inlined,at_inlined

// VC: replay: assert-failed{{$}}
// VC-NEXT: replay: assert-at: f.c:3:5 inlined-at=g.c:9:2,main.c:20:7{{$}}
// VC: replay: assert-op: f.c:5:3 inlined-at=g.c:9:2,main.c:20:7 name=claim.ne7{{$}}
// VC-NOT: llvm @
// VCJSON: "kind":"replay"
// VCJSON-SAME: "at":"f.c:3:5","at_inlined":["g.c:9:2","main.c:20:7"]
// VCJSON-SAME: "op":"f.c:5:3","op_inlined":["g.c:9:2","main.c:20:7"],"op_name":"claim.ne7"
// INTERP: print.object: stack.1 kind=stack size=8 at=f.c:2:8 inlined-at=g.c:9:2,main.c:20:7 at_name=buf{{$}}
// INTERP: print: x = 7 : {{u?}}i8 at=f.c:4:3 inlined-at=g.c:9:2,main.c:20:7 at_name=show.x{{$}}
// INTERP: status: assert-failed
// INTERP-NEXT: at: loc("claim.ne7"(fused<"llvm @main %entry #7">[callsite("f.c":5:3 at callsite("g.c":9:2 at "main.c":20:7))])){{$}}
// INTERP-NEXT: assert-at: f.c:3:5 inlined-at=g.c:9:2,main.c:20:7{{$}}
// INTERP-NEXT: assert-op: f.c:5:3 inlined-at=g.c:9:2,main.c:20:7 name=claim.ne7{{$}}
// IJSON: "kind":"print.object"
// IJSON-SAME: "at":"f.c:2:8","at_inlined":["g.c:9:2","main.c:20:7"],"at_name":"buf"
// IJSON: "kind":"print","path":"x"
// IJSON-SAME: "at":"f.c:4:3","at_inlined":["g.c:9:2","main.c:20:7"],"at_name":"show.x"
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @inl {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("main.c":19:11)
  %c7 = sea.const 7 : i8
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64> loc("buf"(callsite("f.c":2:8 at callsite("g.c":9:2 at "main.c":20:7))))
  %m0 = sea.mem.nondet "region" [2] : !m
  %m1 = sea.store %m0[%p], %x inplace : !m, i8
  %ok = sea.icmp ne, %x, %c7 : i8 loc(callsite(callsite("f.c":3:5 at "g.c":9:2) at "main.c":20:7))
  sea.print "p" %p : !sea.ptr<64>
  sea.print "x" %x : i8 loc("show.x"(fused<"llvm @main %entry #6">[callsite("f.c":4:3 at callsite("g.c":9:2 at "main.c":20:7))]))
  sea.assert %ok loc("claim.ne7"(fused<"llvm @main %entry #7">[callsite("f.c":5:3 at callsite("g.c":9:2 at "main.c":20:7))]))
  sea.halt
}

// `assert false` reached by branches: the last located op is an inlined
// branch; the assert's location is only the llvm link, which gets its
// position in this file.
// PATH: status: assert-failed
// PATH-NEXT: at: loc(fused<"llvm @main %err #1">["{{.*}}inlined-locations.mlir":[[#OP:@LINE+20]]:3]){{$}}
// PATH-NEXT: assert-at: f.c:7:9 inlined-at=main.c:21:3{{$}}
// PATH-NEXT: assert-op: {{.*}}inlined-locations.mlir:[[#OP]]:3{{$}}
// PATH-NEXT: assert-path: f.c:7:9 inlined-at=main.c:21:3 <- main.c:19:3{{$}}
// PJSON: "kind":"status"
// PJSON-SAME: "at":"f.c:7:9","at_inlined":["main.c:21:3"]
// PJSON-SAME: "op":"{{.*}}inlined-locations.mlir:[[#OP:@LINE+14]]:3"
// PJSON-NOT: op_inlined
// PJSON-SAME: "path":["f.c:7:9","main.c:19:3"],"path_inlined":{{\[\["main.c:21:3"\],\[\]\]}}
sea.program @path {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c1 = sea.const 1 : i8
  sea.br ^check loc("main.c":19:3)
^check:
  %ne = sea.icmp ne, %x, %c1 : i8
  sea.cond_br %ne, ^ok, ^err loc(fused<"llvm @main %check #1">[callsite("f.c":7:9 at "main.c":21:3)])
^ok:
  sea.halt
^err:
  %f = sea.const 0 : i1 loc(fused<"llvm @main %err #0">[])
  sea.assert %f loc(fused<"llvm @main %err #1">[])
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
