// --mem-trace (an alias of --log=vc.mem): a memory heartbeat on stderr. A
// beat every --mem-trace-interval seconds while Expr nodes are made (0: at
// every check, here every 16 nodes: --mem-watch-step, testing only) and a
// phase-end line per phase: memory, live and made nodes, the rewriter's
// cache and rule calls, the op being encoded, and the top lists (count
// first: an operator name may hold a colon). Shapes only: no count, time
// or size is pinned.
// RUN: sea-vc %s --lower --seam --mem-trace --mem-trace-interval=0 --mem-watch-step=16 2>&1 >/dev/null | FileCheck %s
// RUN: sea-vc %s --lower --seam --log=vc.mem --mem-trace-interval=0 --mem-watch-step=16 2>&1 >/dev/null | FileCheck %s
// CHECK: log: vc.mem beat phase=encode run=1 time={{[0-9.]+}}{{.*}} nodes-live={{[0-9]+}} nodes-made={{[0-9]+}} rw-cache={{[0-9]+}} rule-calls={{[0-9]+}} op=sea.{{[a-z.]+}} op-at={{.+}} op-nodes={{[0-9]+}} top-ops={{"?}}{{[0-9]+}}:{{.+}} top-rules={{.+}} top-sites={{[0-9]+}}:sea.{{[a-z.]+}}@{{.+}}
// The load through the stored pointer is rewritten in its op: a beat
// inside it names it as the op in progress.
// Its rules show up in the beats after it (fired since the last beat), and
// it is the op that made the most nodes in encode.
// CHECK: log: vc.mem beat phase=encode run=1 {{.*}} op=sea.load op-at=f.c:9:3
// CHECK: log: vc.mem beat phase=encode {{.*}} top-rules={{[^ ]*}}row.
// CHECK: log: vc.mem phase-end phase=encode run=1 {{.*}} top-sites={{[0-9]+}}:sea.load@f.c:9:3,
// CHECK: log: vc.mem phase-end phase=solve

// Off by default and with --mem-trace=false; nothing else on stdout
// changes (the query included).
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-vc %s --lower --seam --mem-trace --mem-trace=false 2>&1 | FileCheck %s --check-prefix=QUIET
// QUIET-NOT: vc.mem
// RUN: sea-vc %s --lower --seam --smt2 > %t.off
// RUN: sea-vc %s --lower --seam --smt2 --mem-trace --mem-trace-interval=0 --mem-watch-step=16 2>/dev/null > %t.on
// RUN: sea-vc %s --lower --seam --smt2 --max-nodes=100000000 --max-rss=100000000 > %t.budget
// RUN: sed -e '/-time:/d' -e '/^tool.version/d' %t.off > %t.off.s
// RUN: sed -e '/-time:/d' -e '/^tool.version/d' %t.on > %t.on.s
// RUN: sed -e '/-time:/d' -e '/^tool.version/d' %t.budget > %t.budget.s
// RUN: diff %t.off.s %t.on.s
// RUN: diff %t.off.s %t.budget.s

// A budget never replaces a verdict: they bound encoding and solving. With
// the limit between the live nodes up to the solve's end and those of the
// replay (measured from this run's phase-end lines by the script), the
// replay passes the limit and the run still prints its one `result: sat`
// and exits with the status of a sat.
// RUN: sh %sea_src_root/test/VC/limits/Inputs/budget-after-verdict.sh %sea_vc %s > %t.verdict
// RUN: FileCheck %s --check-prefix=VERDICT < %t.verdict
// VERDICT: result: sat
// VERDICT-NOT: result:
// VERDICT-NOT: budget

// jsonl: the log events are JSON objects with the same fields.
// RUN: sea-vc %s --lower --seam --mem-trace --mem-trace-interval=0 --mem-watch-step=16 --format=jsonl 2>&1 >/dev/null | FileCheck %s --check-prefix=JSONL
// JSONL: {"log":"vc.mem","level":1,"msg":"beat","phase":"encode","run":1,"time":{{.*}}"nodes-live":{{[0-9]+}},{{.*}}"top-ops":"{{.+}}","text":"log: vc.mem beat
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
