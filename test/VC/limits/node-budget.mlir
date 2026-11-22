// --max-nodes: a budget of live Expr nodes, checked as nodes are made
// (every max/8 nodes, so deterministic): exceeded, the run stops with
// `result: unknown`, `budget: hit resource=nodes` and the census of that moment
// (budget.* lines: the op being encoded, the operators with the most live
// nodes, the rules by firings, the sea ops by nodes made), error
// vc.node-budget, status 3. Shapes only: which op or rule leads is not
// pinned.
// RUN: not sea-vc %s --lower --seam --max-nodes=150 --brunch-stats 2>%t.err | FileCheck %s
// RUN: FileCheck %s --check-prefix=ERR < %t.err
// CHECK: result: unknown
// CHECK-NEXT: budget: hit resource=nodes limit=150 nodes-live={{1[5-6][0-9]}} nodes-made={{[0-9]+}}{{( rss-mb=[0-9]+ vm-mb=[0-9]+)?}} peak-rss-mb={{[0-9]+}} phase=encode run=1 time={{[0-9.]+}} rw-cache={{[0-9]+}} rule-calls={{[0-9]+}}{{$}}
// CHECK-NEXT: budget.op-in-progress: sea.{{[a-z.]+}} nodes={{[0-9]+}} at={{.+}}{{$}}
// CHECK-NEXT: budget.op: {{.+}} live={{[0-9]+}}{{$}}
// CHECK: budget.site: sea.{{[a-z.]+}} nodes={{[0-9]+}} at={{.+}}{{$}}
// CHECK: BRUNCH_STAT result unknown
// CHECK-NEXT: BRUNCH_STAT budget nodes
// CHECK-NEXT: BRUNCH_STAT status 3
// CHECK-NOT: result:
// ERR: Error: more than 150 live Expr nodes (--max-nodes) in phase encode: stopped, the census is in the budget.* lines [vc.node-budget]
// RUN: sh -c 'sea-vc "$0" --lower --seam --max-nodes=150 >/dev/null 2>&1; test $? -eq 3' %s

// The rules: with a budget hit after the load through the stored pointer,
// the census has the rules that rewrote it, by total firings; with
// --seam-stats the rewriter's counters follow.
// RUN: not sea-vc %s --lower --seam --max-nodes=230 --seam-stats 2>/dev/null | FileCheck %s --check-prefix=RULES
// RULES: budget: hit resource=nodes limit=230
// RULES: budget.rule: {{[a-z.-]+}} fired={{[0-9]+}} recent={{[0-9]+}}{{$}}
// RULES: budget.site: sea.load nodes={{[0-9]+}} at=f.c:9:3{{$}}
// RULES: seam.rule-calls: {{[0-9]+}}
// RULES: seam.rule.

// The same budget stops the same way twice (times and memory aside).
// RUN: not sea-vc %s --lower --seam --max-nodes=150 2>/dev/null | sed -e 's/ time=[0-9.]*//' -e 's/ rss-mb=[0-9]*//' -e 's/ vm-mb=[0-9]*//' -e 's/ peak-rss-mb=[0-9]*//' -e '/-time:/d' > %t.a
// RUN: not sea-vc %s --lower --seam --max-nodes=150 2>/dev/null | sed -e 's/ time=[0-9.]*//' -e 's/ rss-mb=[0-9]*//' -e 's/ vm-mb=[0-9]*//' -e 's/ peak-rss-mb=[0-9]*//' -e '/-time:/d' > %t.b
// RUN: diff %t.a %t.b

// jsonl: every line one JSON object, the same text as the text format.
// RUN: not sea-vc %s --lower --seam --max-nodes=150 > %t.txt
// RUN: not sea-vc %s --lower --seam --max-nodes=150 --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl --require stat:resource,limit,nodes-live,phase
// RUN: FileCheck %s --check-prefix=JSONL < %t.jsonl
// JSONL: {"kind":"result","value":"unknown","text":"result: unknown"}
// JSONL-NEXT: {"kind":"stat","key":"budget","value":"hit","resource":"nodes","limit":150,

// --max-rss: a budget of resident memory in MiB, checked at the same
// points (1 MiB: exceeded at the first check).
// RUN: not sea-vc %s --lower --seam --max-rss=1 2>%t.rss.err | FileCheck %s --check-prefix=RSS
// RUN: FileCheck %s --check-prefix=RSSERR < %t.rss.err
// RSS: result: unknown
// RSS-NEXT: budget: hit resource=rss limit=1
// RSSERR: [vc.rss-budget]
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %p = sea.load %m0[%A] : !m -> !sea.ptr<64>
  %y = sea.load %m0[%B] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %x = sea.load %m1[%B] : !m -> i8 loc("f.c":9:3)
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
