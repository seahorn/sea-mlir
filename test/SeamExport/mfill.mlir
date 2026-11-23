// sea-seam-export of a constant-data fill (docs/design/MemFill.md, "SeaM";
// docs/SeamExport.md): in a SeaM lane the fill is the node mfill (memory,
// destination; data: the bytes), seam-lean's RangeExpr.mfill; a read at a
// numeral offset in the data is the byte (row.mfill-hit), past it the
// memory below (row.mfill-skip). Without rules (raw) the boundary
// translates a read by the read equation: (j - d) <u n, the lookup of j - d
// in the data, the read below. A lane without SeaM refuses the fill (sea-vc
// --lower expands it first).
// RUN: sea-seam-export %s --lane=seam-inb0 --firings | %python %S/Inputs/show.py | FileCheck %s
// RUN: sea-seam-export %s --lane=seam-inb0 | %python -c "import json,sys; d=json.load(sys.stdin); t=[r['term'] for r in d['roots'] if r['name']=='%%m1'][0]; print('node', t['op'], t['sort'], len(t['args']), t['data'])" | FileCheck %s --check-prefix=NODE
// RUN: sea-seam-export %s --lane=raw | %python %S/Inputs/show.py | FileCheck %s --check-prefix=RAW
// RUN: not sea-seam-export %s --lane=plain 2>&1 | FileCheck %s --check-prefix=PLAIN
// CHECK: header: schema=5 lane=seam-inb0
// CHECK: root %r1 sea.load bv: 2
// CHECK: root %r3 sea.load bv: select(constMem(0), index(segment(obj.s, 8), c=3, K=0, plain))
// CHECK-NOT: erased {{.*}}mfill
// CHECK: firing {{.*}}rules=row.mfill-hit
// NODE: node mfill memory 2 [1, 2, 3]
// RAW: erased %r1: ite(ult(sub(add(obj.s, 1), obj.s), 3), ite(eq(sub(add(obj.s, 1), obj.s), 0), 1, ite(eq(sub(add(obj.s, 1), obj.s), 1), 2, 3)), select(constMem(0), add(obj.s, 1)))
// PLAIN: [vc.mem-fill]
sea.program @main {
  %zero = sea.const 0 : i8 loc("zero")
  %m0 = sea.mem.const %zero : !sea.mem<!sea.addr<8>, i8> loc("m0")
  %s = sea.segment "s" base 8 : i8 size 8 : i8 align 1 kind "stack" : !sea.addr<8> loc("s")
  %one = sea.const 1 : i8 loc("one")
  %three = sea.const 3 : i8 loc("three")
  %m1 = sea.mem.fill %m0[%s], dense<[1, 2, 3]> : tensor<3xi8> inplace : !sea.mem<!sea.addr<8>, i8> loc("m1")
  %s1 = sea.addr.add %s, %one : !sea.addr<8>, i8 loc("s1")
  %s3 = sea.addr.add %s, %three : !sea.addr<8>, i8 loc("s3")
  %r1 = sea.load %m1[%s1] : !sea.mem<!sea.addr<8>, i8> -> i8 loc("r1")
  %r3 = sea.load %m1[%s3] : !sea.mem<!sea.addr<8>, i8> -> i8 loc("r3")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
