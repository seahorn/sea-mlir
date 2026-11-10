// Level-1 flow events of sea-vc (docs/reference/logging.md): vc.phase,
// lower.pass, vc.solve. Shapes only: no time or size is pinned.

// vc.phase: a start and an end per phase; the end has the time and the
// phase's sizes; replay's end has the result of the validation.
// RUN: sea-vc %s --lower --seam --log=vc.phase 2>&1 >/dev/null | FileCheck %s --check-prefix=PHASE
// PHASE: log: vc.phase start phase=lower
// PHASE-NEXT: log: vc.phase end phase=lower time={{[0-9.]+}} ops-before={{[0-9]+}} ops-after={{[0-9]+}}
// PHASE-NEXT: log: vc.phase start phase=encode run=1{{$}}
// PHASE-NEXT: log: vc.phase end phase=encode time={{[0-9.]+}} nodes-made={{[0-9]+}} sides={{[0-9]+}} guarantees={{[0-9]+}}
// PHASE-NEXT: log: vc.phase start phase=simplify
// PHASE-NEXT: log: vc.phase end phase=simplify time={{[0-9.]+}} nodes-made={{[0-9]+}}
// PHASE-NEXT: log: vc.phase start phase=solver-term
// PHASE-NEXT: log: vc.phase end phase=solver-term time={{[0-9.]+}} nodes-made={{[0-9]+}}
// PHASE-NEXT: log: vc.phase start phase=ackermann
// PHASE-NEXT: log: vc.phase end phase=ackermann time={{[0-9.]+}} on={{true|false}} arrays={{[0-9]+}} pairs={{[0-9]+}} nodes-made={{[0-9]+}}
// PHASE-NEXT: log: vc.phase start phase=solve
// PHASE-NEXT: log: vc.phase end phase=solve time={{[0-9.]+}}{{$}}
// PHASE-NEXT: log: vc.phase start phase=replay
// PHASE-NEXT: log: vc.phase end phase=replay time={{[0-9.]+}} status=assert-failed checked={{[0-9]+}} mismatches=0 validated=true{{$}}
// PHASE-NOT: log:

// lower.pass: one event per pass of the lowering, between lower's start
// and end.
// RUN: sea-vc %s --lower '--log=vc.phase,lower.pass' 2>&1 >/dev/null | FileCheck %s --check-prefix=PASS
// PASS: log: vc.phase start phase=lower
// PASS-NEXT: log: lower.pass pass=sea-apply-tracking on=sea.program program=@main ops-before={{[0-9]+}} ops-after={{[0-9]+}} time={{[0-9.]+}}{{$}}
// PASS: log: lower.pass pass=sccp on=sea.program
// PASS: log: lower.pass pass=sea-split-critical-edges on=sea.program
// PASS-NEXT: log: vc.phase end phase=lower
// PASS-NOT: lower.pass
// No lowering, no pass events.
// RUN: sea-vc %s --log=lower.pass 2>&1 | FileCheck %s --check-prefix=NOPASS --implicit-check-not=log:
// NOPASS: result: sat

// vc.solve: one event per solver call, with the backend and the result.
// RUN: sea-vc %s --log=vc.solve 2>&1 | FileCheck %s --check-prefix=SOLVE --implicit-check-not=log:
// SOLVE: log: vc.solve backend=z3 result=sat time={{[0-9.]+}} nodes-made={{[0-9]+}}{{$}}
// SOLVE: result: sat
// RUN: %if bitwuzla %{ sea-vc %s --solver=bitwuzla --log=vc.solve 2>&1 | FileCheck %s --check-prefix=BZLA %}
// BZLA: log: vc.solve backend=bitwuzla result=sat

// Several programs are lowered in parallel: each event names its program.
// A VC run that starts again (here the --ack-stride retry) numbers its
// encode: run=2.
// RUN: sea-vc %S/../VC/seam/ack-stride.mlir --program=wrap --lower --seam --ack-stride '--log=vc.phase,lower.pass' 2>&1 >/dev/null | FileCheck %s --check-prefix=MULTI
// MULTI-DAG: log: lower.pass pass=sccp on=sea.program program=@wrap ops-before=
// MULTI-DAG: log: lower.pass pass=sccp on=sea.program program=@apart ops-before=
// MULTI: log: vc.phase start phase=encode run=1{{$}}
// MULTI: log: vc.phase start phase=encode run=2{{$}}

// *:1 is the whole flow; the stdout records do not change.
// RUN: sea-vc %s --lower '--log=*:1' 2>&1 >/dev/null | FileCheck %s --check-prefix=ALL
// ALL: log: vc.phase start phase=lower
// ALL: log: lower.pass
// ALL: log: vc.phase start phase=solve{{$}}
// ALL-NEXT: log: vc.solve backend=z3 result=sat
// ALL-NEXT: log: vc.phase end phase=solve
// RUN: sea-vc %s --lower > %t.off
// RUN: sea-vc %s --lower '--log=*:1' 2>/dev/null > %t.on
// RUN: sed -e '/-time:/d' -e '/^tool.version/d' %t.off > %t.off.s
// RUN: sed -e '/-time:/d' -e '/^tool.version/d' %t.on > %t.on.s
// RUN: diff %t.off.s %t.on.s

// jsonl: the same events, one object each.
// RUN: sea-vc %s --format=jsonl --log=vc.phase 2>&1 >/dev/null | FileCheck %s --check-prefix=JSONL
// JSONL: {"log":"vc.phase","level":1,"msg":"start","phase":"encode","run":1,"text":"log: vc.phase start phase=encode run=1"}
// JSONL-NEXT: {"log":"vc.phase","level":1,"msg":"end","phase":"encode","time":{{[0-9.]+}},"nodes-made":{{[0-9]+}},

sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c3 = sea.const 3 : i8
  %ne = sea.icmp ne, %x, %c3 : i8
  sea.assert %ne
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
