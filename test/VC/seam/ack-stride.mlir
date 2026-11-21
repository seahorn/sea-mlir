// RUN: sea-vc %s --program=apart --lower --seam --seam-stats --ack-stride | FileCheck %s --check-prefix=APART
// RUN: sea-vc %s --program=apart --lower --seam --seam-stats --ack-stride=false | FileCheck %s --check-prefix=OFF
// RUN: bash -c 'sea-vc %s --program=wrap --lower --seam --seam-stats --ack-stride --log=vc.phase > %t.wrap 2>&1; test $? -eq 4'
// RUN: FileCheck %s --check-prefix=WRAP < %t.wrap
// RUN: bash -c 'sea-vc %s --program=wrapmem --lower --seam --ack-stride > %t.wrapmem 2>&1; test $? -eq 4'
// RUN: FileCheck %s --check-prefix=WRAPMEM < %t.wrapmem
// RUN: bash -c 'sea-vc %s --program=mask --lower --seam > %t.mask 2>%t.mask.err; test $? -eq 4'
// RUN: FileCheck %s --check-prefix=MASK < %t.mask
// RUN: FileCheck %s --check-prefix=MASKERR < %t.mask.err
// RUN: bash -c 'sea-vc %s --program=mask --lower --seam --format=jsonl > %t.mask.jsonl 2>/dev/null; test $? -eq 4'
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.mask --jsonl %t.mask.jsonl
// RUN: FileCheck %s --check-prefix=MASKJ < %t.mask.jsonl
// RUN: sea-vc %s --program=reach --lower --seam --reach --log=vc.phase 2>&1 | FileCheck %s --check-prefix=REACH
// RUN: sea-vc %s --program=wide --lower --seam --seam-wide --seam-stats --ack-stride | FileCheck %s --check-prefix=WIDE

// RUN: sea-vc %s --program=nuw --lower --seam --seam-stats --ack-stride | FileCheck %s --check-prefix=NUW
// RUN: sea-vc %s --program=negstride --lower --seam --seam-stats --ack-stride | FileCheck %s --check-prefix=NUW
// RUN: sea-vc %s --program=mixed --lower --seam --seam-stats --ack-stride | FileCheck %s --check-prefix=MIXED
// RUN: sea-vc %s --program=plain --lower --seam --seam-stats --ack-stride | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --program=split --lower --seam --seam-stats --ack-stride | FileCheck %s --check-prefix=SPLIT
// The stride rule in the Ackermann lowering (docs/design/PtrIndex.md
// section 5). @apart: reads of the initial memory at e[i].f (stride 24,
// field 8) and e[j].g (field 16), inbounds: 24i + 8 = 24j + 16 has no
// integer solution, so the pair gets no constraint (off: one constraint).
// @wrap: the program assumes the two addresses equal, which only a wrapping
// offset gives (24 (i - j) = 8 mod 2^64 has a solution; it breaks the
// promise). With the rule the lowered query is sat with a model that breaks
// the promise. The promise is a trusted front-end fact, so that model is
// the answer: sat, status 4, a run that breaks the promise instead of
// failing the assertion (docs/design/PtrIndex.md section 5). No rerun: one
// encode (without the rule the answer is unsat: equal addresses read one
// cell).
// APART: vc.ackermann-reads: 2
// APART: vc.ackermann-stride-pairs: 1
// APART: vc.ackermann-stride-a-null-pairs: 0
// APART: vc.ackermann-stride-a-nowrap-pairs: 0
// APART: vc.ackermann-constraints: 0
// APART: result: sat
// OFF: vc.ackermann-constraints: 1
// OFF-NOT: stride-pairs
// WRAP: vc.phase start phase=encode run=1
// WRAP-NOT: run=2
// WRAP: vc.ackermann-stride-broken: 1
// WRAP-NOT: fallback
// WRAP: result: sat
// WRAP-NOT: result:
// WRAP: vc.ack-stride-promise-broken: {{.*}}ack-stride.mlir:[[#GEP:]]:{{[0-9]+}} promise=P{{[123]}} what=inbounds breaks={{[1-9][0-9]*}} via-memory=0
// WRAP-NEXT: vc.ack-stride-promise-broken-access: sea.load {{.*}}ack-stride.mlir:
// WRAP: Error: {{.*}}ack-stride.mlir:[[#GEP]]:{{.*}}[vc.ack-stride-promise-broken]
// WRAP-NOT: run=2
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @apart {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @wrap {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %ai = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %bi = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %same = sea.icmp eq, %ai, %bi : i64
  sea.assume %same
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
// @wrapmem: @wrap with both GEP pointers stored to memory and loaded back
// before the loads through them. The interpreter reports the broken promise
// at the access, through memory (docs/design/PtrIndex.md section 6); the
// classification takes it from there: status 4, via-memory=1, the access
// named.
// WRAPMEM: vc.ackermann-stride-broken: 1
// WRAPMEM: result: sat
// WRAPMEM: vc.ack-stride-promise-broken: {{.*}}ack-stride.mlir:{{.*}} via-memory=1
// WRAPMEM-NEXT: vc.ack-stride-promise-broken-access: sea.load {{.*}}ack-stride.mlir:
// WRAPMEM: (the pointer went through memory){{.*}}[vc.ack-stride-promise-broken]
sea.program @wrapmem {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %z = sea.const 0 : i8
  %s0 = sea.mem.const %z : !pm
  %n = sea.const 96 : i64
  %c16 = sea.const 16 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %slot = sea.malloc %c16 align 8 [4] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %ai = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %bi = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %same = sea.icmp eq, %ai, %bi : i64
  sea.assume %same
  %slot2 = sea.ptr.index %slot [] strides [] offset 8 : !sea.ptr<64>
  %s1 = sea.store %s0[%slot], %a inplace align 8 : !pm, !sea.ptr<64>
  %s2 = sea.store %s1[%slot2], %b inplace align 8 : !pm, !sea.ptr<64>
  %ra = sea.load %s2[%slot] align 8 : !pm -> !sea.ptr<64>
  %rb = sea.load %s2[%slot2] align 8 : !pm -> !sea.ptr<64>
  %x = sea.load %m0[%ra] : !pm -> i8
  %y = sea.load %m0[%rb] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
// @mask: @wrap's colliding pair, and a load of the earlier memory version
// %r0 after a store to the cell it reads: the replay's mismatch is also
// explained by the program's Mem-SSA no-clobber claim (status 2 on its own).
// The run breaks a GEP promise at an access, which is the answer: status 4,
// and both facts are printed (docs/design/PtrIndex.md section 5).
// MASK: result: sat
// MASK: replay: no-clobber-violated: {{.*}}ack-stride.mlir:
// MASK: vc.ack-stride-promise-broken: {{.*}}ack-stride.mlir:{{[0-9]+}}:{{[0-9]+}} promise=P1 what=inbounds breaks={{[1-9][0-9]*}} via-memory=0
// MASK-NEXT: vc.ack-stride-promise-broken-access: sea.load {{.*}}ack-stride.mlir:
// MASKERR: [vc.ack-stride-promise-broken]
// MASKERR-NOT: [vc.replay-mismatch]
// MASKJ: "key":"vc.ack-stride-promise-broken","value":"{{.*}}ack-stride.mlir:{{[0-9:]+}}","promise":"P1","what":"inbounds","breaks":{{[0-9]+}},"via-memory":0
sea.program @mask {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %r0 = sea.mem.nondet "r" [4] : !pm
  %n = sea.const 96 : i64
  %c1 = sea.const 1 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %c1 align 1 [5] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %ai = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %bi = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %same = sea.icmp eq, %ai, %bi : i64
  sea.assume %same
  %seven = sea.const 7 : i8
  %r1 = sea.store %r0[%q], %seven inplace : !pm, i8
  %z = sea.load %r0[%q] : !pm -> i8
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %e1 = sea.icmp eq, %x, %y : i8
  %e2 = sea.icmp eq, %z, %seven : i8
  %ok = sea.or %e1, %e2 : i1
  sea.assert %ok
  sea.halt
}
// @reach, with --reach: the target in ^t needs the two reads to differ,
// which only a model that breaks the GEP promise gives. --reach keeps the
// rerun without the rule (no answer to classify per target): the second
// run decides ^t unreachable.
// REACH: vc.phase start phase=encode run=1
// REACH: [vc.ack-stride-reach-retry]
// REACH: vc.phase start phase=encode run=2
// REACH: vc.ackermann-stride: fallback
// REACH: reach: ack-stride.mlir:[[#@LINE+20]]:3 unreachable op=sea.assume
// REACH: reach.summary: targets=3 reachable=2 unreachable=1 unknown=0
sea.program @reach {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %ai = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %bi = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %same = sea.icmp eq, %ai, %bi : i64
  sea.assume %same
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %ne = sea.icmp ne, %x, %y : i8
  sea.cond_br %ne, ^t, ^e
^t:
  %k = sea.nondet "k" [4] : i1
  sea.assume %k
  sea.br ^j
^e:
  sea.br ^j
^j:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}
// @wide, with --seam-wide: 8-byte reads at e[i].f and e[j].g. Byte k of each
// is the node with K = k (the K writers); 24 never divides 8 + K_g - K_f
// for K in [0, 8), so no byte pair keeps a constraint (the exact tests
// decide some first; the stride rule the rest, under the wide accesses'
// A-NOWRAP too: byte K != 0, counted stride-a-nowrap-pairs).
// WIDE: vc.ackermann-stride-pairs: {{[1-9][0-9]*}}
// WIDE: vc.ackermann-stride-a-nowrap-pairs: {{[1-9][0-9]*}}
// WIDE: vc.ackermann-constraints: 0
// WIDE: result: sat
sea.program @wide {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i64
  %y = sea.load %m0[%b] : !pm -> i64
  %ok = sea.icmp eq, %x, %y : i64
  sea.assert %ok
  sea.halt
}

// @nuw: the unsigned promise; @negstride: stride -24 (a field read from the
// end of an array backwards). Both apart.
// NUW: vc.ackermann-stride-pairs: 1
// NUW: vc.ackermann-constraints: 0
// @mixed: an inbounds and a nuw node read the index as different integers:
// not compared (other-base), the constraint stays.
// MIXED: vc.ackermann-stride-pairs: 0
// MIXED: vc.ackermann-stride-other-base: 1
// MIXED: vc.ackermann-constraints: 1
// @plain: one read at an address computed as an integer (ptr.add of
// 24 * i + 8): not eligible, the constraint stays.
// PLAIN: vc.ackermann-stride-pairs: 0
// PLAIN: vc.ackermann-stride-not-eligible: 1
// PLAIN: vc.ackermann-constraints: 1
// @split: 8-byte reads, split into bytes by sea-split-wide-access (ptr.add
// of a numeral to the node): each byte is the node with K = k (OpSem's
// addr.add K writer), so no constraint stays.
// SPLIT: vc.ackermann-stride-pairs: {{[1-9][0-9]*}}
// SPLIT: vc.ackermann-constraints: 0
sea.program @nuw {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 nuw : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 nuw : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @negstride {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [-24] offset 80 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [-24] offset 88 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @mixed {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 nuw : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @plain {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %c24 = sea.const 24 : i64
  %c8 = sea.const 8 : i64
  %o = sea.mul %i, %c24 : i64
  %o8 = sea.add %o, %c8 : i64
  %a = sea.ptr.add %p, %o8 : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @split {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i64
  %y = sea.load %m0[%b] : !pm -> i64
  %ok = sea.icmp eq, %x, %y : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
