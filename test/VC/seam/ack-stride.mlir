// RUN: sea-vc %s --program=apart --lower --seam --seam-stats --ack-stride | FileCheck %s --check-prefix=APART
// RUN: sea-vc %s --program=apart --lower --seam --seam-stats --ack-stride=false | FileCheck %s --check-prefix=OFF
// RUN: sea-vc %s --program=wrap --lower --seam --seam-stats --ack-stride 2>&1 | FileCheck %s --check-prefix=WRAP
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
// the promise; sea-vc runs again without the rule and answers unsat (equal
// addresses read one cell).
// APART: vc.ackermann-reads: 2
// APART: vc.ackermann-stride-pairs: 1
// APART: vc.ackermann-stride-a-inb-pairs: 0
// APART: vc.ackermann-constraints: 0
// APART: result: sat
// OFF: vc.ackermann-constraints: 1
// OFF-NOT: stride-pairs
// The decision comes before any result is printed: no `result: sat`.
// WRAP-NOT: result: sat
// WRAP: vc.ackermann-stride-broken: 1
// WRAP-NOT: result: sat
// WRAP: vc.ackermann-stride: fallback
// WRAP-NOT: result: sat
// WRAP: result: unsat
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
// @wide, with --seam-wide: 8-byte reads at e[i].f and e[j].g. Byte k of each
// is the node with K = k (the K writers); 24 never divides 8 + K_g - K_f
// for K in [0, 8), so no byte pair keeps a constraint (the exact tests
// decide some first; the stride rule the rest, under the wide accesses'
// A-INB too).
// WIDE: vc.ackermann-stride-pairs: {{[1-9][0-9]*}}
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
