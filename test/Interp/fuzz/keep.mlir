// --fuzz-keep: why a trial joins the corpus. Default blocks (new block, edge
// or passed assume); cmp adds trials that reach a new comparison bucket,
// frontier trials that pass more assumes than any corpus entry. fuzz.corpus
// counts the kept trials per reason, and --fuzz-corpus-max drops (and
// counts) kept trials past the bound.
// RUN: rm -rf %t && mkdir -p %t
// RUN: sea-run %s --program=cmp --fuzz=3000 --seed=1 | FileCheck %s --check-prefix=BLOCKS
// RUN: sea-run %s --program=cmp --fuzz=3000 --seed=1 --fuzz-keep=blocks,cmp --fuzz-out=%t/cmp | FileCheck %s --check-prefix=CMP
// RUN: ls %t/cmp | FileCheck %s --check-prefix=CMPFILES
// RUN: cat %t/cmp/cov-cmp-*.txt | FileCheck %s --check-prefix=CMPHEAD
// RUN: sea-run %s --program=cmp --fuzz=3000 --seed=1 --fuzz-keep=all --fuzz-corpus-max=2 | FileCheck %s --check-prefix=MAX
// RUN: sea-run %s --program=gates --fuzz=20000 --seed=1 --fuzz-keep=frontier --fuzz-out=%t/fr | FileCheck %s --check-prefix=FRONTIER
// RUN: ls %t/fr | FileCheck %s --check-prefix=FRFILES
// RUN: sea-run %s --program=deep --fuzz=200000 --seed=3 --fuzz-dict=false --fuzz-keep=frontier --fuzz-out=%t/deep --fuzz-corpus-max=1 | FileCheck %s --check-prefix=DEEP
// RUN: sea-run %s --program=cmp --fuzz=2000 --seed=1 --fuzz-keep=all --fuzz-corpus-max=3 > %t.txt
// RUN: sea-run %s --program=cmp --fuzz=2000 --seed=1 --fuzz-keep=all --fuzz-corpus-max=3 --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl --require stat:keep,kept-cmp,max,dropped
// RUN: not sea-run %s --program=cmp --fuzz=10 --fuzz-keep=cmp --fuzz-cmp=false 2>&1 | FileCheck %s --check-prefix=NOCMP
// RUN: not sea-run %s --program=cmp --fuzz=10 --fuzz-keep=blocks,bogus 2>&1 | FileCheck %s --check-prefix=BOGUS

// The default keeps for blocks only.
// BLOCKS: fuzz.corpus: [[N:[0-9]+]] {{.*}} keep=blocks kept-blocks=[[N]] kept-cmp=0 kept-frontier=0 dropped=0

// x is compared with 100: blocks saturate after two entries, buckets need
// more (both ends of the -L..+L range).
// CMP: fuzz.corpus: {{[0-9]+}} {{.*}} keep=blocks,cmp kept-blocks={{[1-9][0-9]*}} kept-cmp={{[1-9][0-9]*}} kept-frontier=0 dropped=0
// CMPFILES: cov-cmp-{{[0-9]+}}.txt
// CMPHEAD: # sea-run --fuzz: trial={{[0-9]+}} status=halted keep=cmp

// MAX: fuzz.corpus: 2 {{.*}} max=2 dropped={{[1-9][0-9]*}}

// Three assumes on three sites: the frontier grows at most three times.
// FRONTIER: fuzz.corpus: [[F:[1-3]]] {{.*}} keep=frontier kept-blocks=0 kept-cmp=0 kept-frontier=[[F]] dropped=0
// FRFILES: cov-frontier-{{[0-9]+}}.txt

// A dropped trial still raises the frontier: with a corpus of one entry the
// kept trials are at most one per depth (three assumes), not one per trial.
// DEEP: fuzz.corpus: 1 {{.*}} files={{[1-4]}} {{.*}} keep=frontier kept-blocks=0 kept-cmp=0 kept-frontier=[[K:[1-3]]] max=1 dropped={{[0-2]}}

// NOCMP: --fuzz-keep=cmp needs the comparison counters
// BOGUS: --fuzz-keep: unknown keep reason 'bogus'

sea.program @cmp {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %lt = sea.icmp ult, %x, %c100 : i8
  sea.cond_br %lt, ^a, ^b
^a:
  sea.br ^done
^b:
  sea.br ^done
^done:
  sea.halt
}

sea.program @gates {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %z = sea.nondet "nondet_u8" [2] : i8
  %c10 = sea.const 10 : i8
  %a = sea.icmp ult, %x, %c10 : i8
  sea.assume %a
  %b = sea.icmp ult, %y, %c10 : i8
  sea.assume %b
  %c = sea.icmp ult, %z, %c10 : i8
  sea.assume %c
  sea.halt
}

sea.program @deep {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %z = sea.nondet "nondet_u8" [2] : i8
  %k = sea.const 90 : i8
  %c = sea.const 17 : i8
  %x1 = sea.xor %x, %k : i8
  %a = sea.icmp eq, %x1, %c : i8
  sea.assume %a
  %y1 = sea.xor %y, %k : i8
  %b = sea.icmp eq, %y1, %c : i8
  sea.assume %b
  %z1 = sea.xor %z, %k : i8
  %d = sea.icmp eq, %z1, %c : i8
  sea.assume %d
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
