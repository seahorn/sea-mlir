// Progress inside encode (vc.phase progress) and the lines a --mem-limit
// hit prints about where it was. Shapes and counts only: no time or memory
// size is pinned.

// --progress-interval=0: one progress event after every block, in encoding
// order; the last one has every block and op done.
// RUN: sea-vc %s --progress --progress-interval=0 2>&1 >/dev/null | FileCheck %s --check-prefix=PROG
// PROG: log: vc.phase start phase=encode run=1{{$}}
// PROG-NEXT: log: vc.phase progress phase=encode run=1 time={{[0-9.]+}} blocks=1 blocks-total=4 ops=3 ops-total=6 nodes-made={{[0-9]+}}{{( rss-mb=[0-9]+ vm-mb=[0-9]+)?}} peak-rss-mb={{[0-9]+}} slowest-op={{sea\.[a-z.]+}} slowest-op-time={{[0-9.]+}} slowest-op-at={{.+}} biggest-op={{sea\.[a-z.]+}} biggest-op-nodes={{[0-9]+}} biggest-op-at={{.+}}{{$}}
// PROG-NEXT: log: vc.phase progress phase=encode run=1 {{.*}} blocks=2 blocks-total=4
// PROG-NEXT: log: vc.phase progress phase=encode run=1 {{.*}} blocks=3 blocks-total=4
// PROG-NEXT: log: vc.phase progress phase=encode run=1 {{.*}} blocks=4 blocks-total=4 ops=6 ops-total=6
// PROG-NEXT: log: vc.phase end phase=encode
// PROG-NOT: progress

// The progress events and a --mem-limit that is not hit change nothing on
// stdout (the query included).
// RUN: sea-vc %s --smt2 > %t.off
// RUN: sea-vc %s --smt2 --progress --progress-interval=0 2>/dev/null > %t.on
// RUN: sea-vc %s --smt2 --mem-limit=64 > %t.mem
// RUN: sed -e '/-time:/d' -e '/^tool.version/d' %t.off > %t.off.s
// RUN: sed -e '/-time:/d' -e '/^tool.version/d' %t.on > %t.on.s
// RUN: sed -e '/-time:/d' -e '/^tool.version/d' %t.mem > %t.mem.s
// RUN: diff %t.off.s %t.on.s
// RUN: diff %t.off.s %t.mem.s

// A --mem-limit hit (planted: the handler runs before encode op n, as when
// an allocation fails there) names the phase and the op being encoded; the
// progress line is the last tick's (none before the first tick).
// RUN: not sea-vc %s --mem-limit=64 --mem-limit-plant=1 --progress-interval=0 2>%t.err | FileCheck %s --check-prefix=HIT1
// RUN: FileCheck %s --check-prefix=HITERR < %t.err
// HIT1: result: unknown
// HIT1-NEXT: mem-limit: hit
// HIT1-NEXT: mem-limit-phase: encode run=1{{$}}
// HIT1-NEXT: mem-limit-op: sea.nondet ops=1 at={{.*}}encode-progress.mlir:{{[0-9]+}}:{{[0-9]+}}{{$}}
// HIT1-NOT: mem-limit-progress
// HITERR: Error: memory limit (--mem-limit) hit [vc.mem-limit]
// RUN: not sea-vc %s --mem-limit=64 --mem-limit-plant=4 --progress-interval=0 | FileCheck %s --check-prefix=HIT4
// HIT4: mem-limit: hit
// HIT4-NEXT: mem-limit-phase: encode run=1{{$}}
// HIT4-NEXT: mem-limit-progress: time={{[0-9.]+}} blocks={{[12]}} blocks-total=4 ops=3 ops-total=6 nodes-made={{[0-9]+}}{{( rss-mb=[0-9]+ vm-mb=[0-9]+)?}} peak-rss-mb={{[0-9]+}} slowest-op=
// HIT4-NEXT: mem-limit-op: sea.add ops=4 at=f.c:7:3{{$}}
// The same through LLVM's out-of-memory path (an LLVM container's failed
// allocation): the same lines, the same status.
// RUN: not sea-vc %s --mem-limit=64 --mem-limit-plant=4 --progress-interval=0 --mem-limit-plant-llvm 2>%t.llvm.err | FileCheck %s --check-prefix=HIT4
// RUN: FileCheck %s --check-prefix=HITERR < %t.llvm.err
// RUN: sh -c 'sea-vc "$0" --mem-limit=64 --mem-limit-plant=4 --progress-interval=0 --mem-limit-plant-llvm >/dev/null 2>&1; test $? -eq 3' %s
// The exit status of a hit is 3.
// RUN: sh -c 'sea-vc "$0" --mem-limit=64 --mem-limit-plant=4 >/dev/null 2>&1; test $? -eq 3' %s

// jsonl: every line one JSON object, the same text as the text format.
// RUN: not sea-vc %s --mem-limit=64 --mem-limit-plant=4 --progress-interval=0 > %t.txt
// RUN: not sea-vc %s --mem-limit=64 --mem-limit-plant=4 --progress-interval=0 --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl --require stat:blocks,ops,nodes-made,peak-rss-mb --require stat:ops,at
// RUN: FileCheck %s --check-prefix=JSONL < %t.jsonl
// JSONL: {"kind":"stat","key":"mem-limit-phase","value":"encode","run":1,"text":"mem-limit-phase: encode run=1"}
// JSONL-NEXT: {"kind":"stat","key":"mem-limit-progress","time":{{[0-9.]+}},"blocks":
// JSONL-NEXT: {"kind":"stat","key":"mem-limit-op","value":"sea.add","ops":4,"at":"f.c:7:3","text":"mem-limit-op: sea.add ops=4 at=f.c:7:3"}

// The plant is for tests only and needs --mem-limit.
// RUN: not sea-vc %s --mem-limit-plant=1 2>&1 | FileCheck %s --check-prefix=NOLIMIT
// NOLIMIT: --mem-limit-plant needs --mem-limit

sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c3 = sea.const 3 : i8
  %c = sea.icmp ult, %x, %c3 : i8
  sea.cond_br %c, ^a, ^b
^a:
  %y = sea.add %x, %c3 : i8 loc("f.c":7:3)
  sea.br ^j(%y : i8)
^b:
  sea.br ^j(%x : i8)
^j(%z: i8):
  %ok = sea.icmp ne, %z, %c3 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
