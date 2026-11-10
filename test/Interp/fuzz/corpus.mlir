// Corpus files in both syntaxes: --input assignments (with comments) and
// the nondet: / memread: lines of a saved sea-run output. Each seed is
// replayed as given: the failing seed's values show in the report.
// RUN: rm -rf %t && mkdir -p %t
// RUN: printf '# a comment\n0=3\n\n1@5=9\n' > %t/1-input.txt
// RUN: sea-run %s --input=0=200,1@5=77 > %t/2-saved.txt
// RUN: sea-run %s --fuzz=1 --corpus=%t/1-input.txt | FileCheck %s --check-prefix=INPUT
// RUN: sea-run %s --fuzz=1 --corpus=%t/2-saved.txt | FileCheck %s --check-prefix=SAVED
// RUN: sea-run %s --input-file=%t/2-saved.txt | FileCheck %s --check-prefix=SAVED
// RUN: printf '0=3\nbogus\n' > %t/bad.txt
// RUN: not sea-run %s --fuzz=1 --corpus=%t/bad.txt 2>&1 | FileCheck %s --check-prefix=BAD
// A generated (not seed) failure: its file holds the memory cell too, and
// replays to the same failure.
// RUN: sea-run %s --fuzz=1000 --seed=1 --fuzz-dict=false --fuzz-out=%t/gen | FileCheck %s --check-prefix=GEN
// RUN: cat %t/gen/failure-*.txt > %t/gen.fail
// RUN: FileCheck %s --check-prefix=GENFILE < %t/gen.fail
// RUN: sea-run %s --input-file=%t/gen.fail | FileCheck %s --check-prefix=GENREPLAY
// RUN: printf '0=300\n' > %t/wide.txt
// RUN: not sea-run %s --fuzz=1 --corpus=%t/wide.txt 2>&1 | FileCheck %s --check-prefix=WIDE

// INPUT: nondet: site=0 callee=nondet_u8 width=8 value=3 origin=input
// INPUT: memread: site=1 addr=5 width=8 value=9 origin=input
// INPUT: status: assert-failed
// INPUT: fuzz.corpus: 1 seeds=1 skipped-lines=0
// SAVED: nondet: site=0 callee=nondet_u8 width=8 value=200 origin=input
// SAVED: memread: site=1 addr=5 width=8 value=77 origin=input
// SAVED: status: assert-failed
// GEN: fuzz.failure: 1 trial={{[0-9]+}} file=
// GEN: fuzz.trials: {{[0-9]+}} seeds=0
// GENFILE: # sea-run --fuzz: trial={{[0-9]+}} status=assert-failed
// GENFILE-DAG: {{^}}0={{[0-9]+$}}
// GENFILE-DAG: {{^}}1@5={{[0-9]+$}}
// GENREPLAY: memread: site=1 addr=5
// GENREPLAY: status: assert-failed
// BAD: bad corpus line {{.*}}bad.txt:2: 'bogus', expected <site>=<value>
// WIDE: corpus value '300' for site 0 is not an i8 value
!m8 = !sea.mem<!sea.addr<8>, i8>
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %m = sea.mem.nondet "m" [1] : !m8
  %c5 = sea.const 5 : i8
  %a = sea.addr.from_int %c5 : i8 to !sea.addr<8>
  %y = sea.load %m[%a] : !m8 -> i8
  %s = sea.add %x, %y : i8
  %c12 = sea.const 12 : i8
  %ok = sea.icmp ult, %s, %c12 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
