// Case splitting (sea-vc --split, docs/design/Split.md) happens before SeaM
// rewrites the uses of the split value, so a part can decide an alias
// question the whole VC leaves open. %q = select(c, A, p) with p loaded from
// nondet memory: a store at %q, then a read at A. Unsplit, the question
// q ~ A is open (the leaf pair p ~ A); with c = 1 (q = A) the read is a
// store hit and no question is left; with c = 0 it stays open.
// --split-check (on by default) proves each level exhaustive and exact.
// Selectors: the VC name (%q, nd.5) or a source line (@sel.cc:768; a whole
// path component: el.cc does not match).
// RUN: sea-vc %s --lower --program=alias --seam --seam-stats | FileCheck %s --check-prefix=WHOLE
// RUN: sea-vc %s --lower --program=alias --seam --split=nd.5 | FileCheck %s --check-prefix=BOOL
// RUN: sea-vc %s --lower --program=alias --seam --split-leaves=%%q | FileCheck %s --check-prefix=LEAVES
// RUN: sea-vc %s --lower --program=alias --seam --split-leaves=@sel.cc:768 | FileCheck %s --check-prefix=LEAVES
// RUN: not sea-vc %s --lower --program=alias --seam --split-leaves=@el.cc:768 2>&1 | FileCheck %s --check-prefix=NOLOC
// RUN: sea-vc %s --lower --program=alias --seam --split-auto | FileCheck %s --check-prefix=AUTO
// NOLOC: [vc.split-target]
// WHOLE: seam.alias.open.all: 1
// WHOLE: result: unsat
// BOOL: split.check: level=0 target=nd.5 cases=2 exhaustive=ok exact=ok
// BOOL: split.part: 0 choice=nd.5=1#0/2 result=unsat {{.*}} alias-open=0 {{.*}} check=ok
// BOOL: split.check: level=0 target=nd.5 cases=2 exhaustive=ok exact=ok
// BOOL: split.part: 1 choice=nd.5=0#1/2 result=unsat {{.*}} alias-open=1 {{.*}} check=ok
// BOOL: split.verdict: unsat parts=2 unsat=2 sat=0 unknown=0
// BOOL: result: unsat
// LEAVES: split.part: 0 choice=%q=leaf:obj.heap.1/paths=1#0/2 result=unsat {{.*}} alias-open=0
// LEAVES: split.part: 1 choice=%q=leaf:unknown(ptr-from-bits)/paths=1#1/2 result=unsat {{.*}} alias-open=1
// LEAVES: result: unsat
// AUTO: split.candidate: name=%q leaves=2 open-aliases=1
// AUTO: split.auto-pick: %q
// AUTO: split.part: 0 {{.*}} alias-open=0
// AUTO: split.part: 1 {{.*}} alias-open=1
// AUTO: split.verdict: unsat parts=2

// --format=jsonl: the split records render to their text lines
// (--vcgen-only: no solve times, so the hardest part is fixed).
// RUN: sea-vc %s --lower --program=alias --seam --split=nd.5 --vcgen-only > %t.txt
// RUN: sea-vc %s --lower --program=alias --seam --split=nd.5 --vcgen-only --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl \
// RUN:   --require split-part-start:part \
// RUN:   --require split-part:part,choice,result,vc_time,solve_time,dag,alias_open,alias_open_pairs,alias_open_memop,rows,ack_reads,ack_pairs,ack_constraints,check \
// RUN:   --require split-verdict:result,parts,unsat,sat,unknown,none,hardest_part,hardest_solve_time \
// RUN:   --require result:value

// A sat part is replayed like any sat: here c = 1 stores 7 at A, and the
// read differs from the value before. The verdict is sat; with
// --split-stop-on-sat the second part is not run.
// RUN: sea-vc %s --lower --program=sat --seam --split=nd.5 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program=sat --seam --split=nd.5 --split-stop-on-sat | FileCheck %s --check-prefix=STOP
// SAT: split.part-result: sat
// SAT: replay: assert-failed
// SAT: validate: checked={{[0-9]+}} mismatches=0
// SAT: split.part: 0 choice=nd.5=1#0/2 result=sat
// SAT: split.part: 1 choice=nd.5=0#1/2 result=sat
// SAT: split.verdict: sat parts=2
// SAT: result: sat
// STOP: split.part: 0 {{.*}} result=sat
// STOP-NOT: split.part: 1
// STOP: split.verdict: sat parts=1 {{.*}} stopped=first-sat
// STOP: result: sat

// The checks catch a broken construction (planted: a lost case, a wrong
// value): exit 5.
// RUN: not sea-vc %s --lower --program=alias --seam --split-leaves=%%q --x-split-plant=drop-case 2>&1 | FileCheck %s --check-prefix=DROP
// RUN: not sea-vc %s --lower --program=alias --seam --split-leaves=%%q --x-split-plant=wrong-value 2>&1 | FileCheck %s --check-prefix=WRONG
// DROP: [vc.split-check-failed]
// DROP: exhaustive=failed
// WRONG: [vc.split-check-failed]
// WRONG: exact=failed

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @alias {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %c = sea.nondet "c" [5] : i1
  %p = sea.load %m0[%C] : !m -> !sea.ptr<64>
  %q = sea.select %c, %A, %p : !sea.ptr<64> loc("q"("src/sel.cc":768:3))
  %y = sea.load %m0[%A] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v inplace : !m, i8
  %x = sea.load %m1[%A] : !m -> i8
  %e1 = sea.icmp eq, %x, %v : i8
  %e2 = sea.icmp eq, %x, %y : i8
  %ok = sea.or %e1, %e2 : i1
  sea.assert %ok
  sea.halt
}
sea.program @sat {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %c = sea.nondet "c" [5] : i1
  %p = sea.load %m0[%C] : !m -> !sea.ptr<64>
  %q = sea.select %c, %A, %p : !sea.ptr<64> loc("q"("src/sel.cc":768:3))
  %y = sea.load %m0[%A] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v inplace : !m, i8
  %x = sea.load %m1[%A] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
