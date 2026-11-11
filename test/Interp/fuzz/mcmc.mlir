// --fuzz-strategy=mcmc: a chain over inputs; a move mutates one value of the
// current input and is accepted by the cost (approach level and the log2 of
// the branch distance of the blocking gate). @narrow has one gate, an
// equality of 3x + 7 with a constant: one input in 2^32 passes it (and
// fails the assert after it), and the dictionary does not hold it. The
// branch distance leads the chain there; the corpus strategy, in the same
// trials and seed, does not get there.
// RUN: rm -rf %t && mkdir -p %t
// RUN: sea-run %s --program=narrow --fuzz=20000 --seed=1 --fuzz-strategy=mcmc --fuzz-gates --fuzz-coverage-out=%t/cov.txt | FileCheck %s --check-prefix=MCMC
// RUN: FileCheck %s --check-prefix=FILE < %t/cov.txt
// RUN: sea-run %s --program=narrow --fuzz=20000 --seed=1 --fuzz-gates --fuzz-coverage-out=%t/cov0.txt | FileCheck %s --check-prefix=CORPUS
// RUN: FileCheck %s --check-prefix=FILE0 < %t/cov0.txt
//
// restarts: after N moves without a new best (0: never)
// RUN: sea-run %s --program=narrow --fuzz=300 --seed=1 --fuzz-strategy=mcmc --fuzz-mcmc-restart=1 | FileCheck %s --check-prefix=RESTART
// RUN: sea-run %s --program=narrow --fuzz=300 --seed=1 --fuzz-strategy=mcmc --fuzz-mcmc-restart=0 | FileCheck %s --check-prefix=NORESTART
//
// share 0: no chain moves, and no draw for the choice: the trials of the
// corpus strategy (the output differs only in the fuzz.mcmc line)
// RUN: sea-run %s --program=safe --fuzz=2000 --seed=3 --fuzz-gates | grep -v -e fuzz.trials: > %t/corpus.txt
// RUN: sea-run %s --program=safe --fuzz=2000 --seed=3 --fuzz-gates --fuzz-strategy=corpus | grep -v -e fuzz.trials: > %t/corpus2.txt
// RUN: sea-run %s --program=safe --fuzz=2000 --seed=3 --fuzz-gates --fuzz-strategy=mcmc --fuzz-mcmc-share=0 | FileCheck %s --check-prefix=SHARE0
// RUN: sea-run %s --program=safe --fuzz=2000 --seed=3 --fuzz-gates --fuzz-strategy=mcmc --fuzz-mcmc-share=0 | grep -v -e fuzz.trials: -e fuzz.mcmc: | sed -e 's/ mcmc-moves=0 mcmc-improved=0//' > %t/share0.txt
// RUN: diff %t/corpus.txt %t/corpus2.txt
// RUN: diff %t/corpus.txt %t/share0.txt
// RUN: sea-run %s --program=safe --fuzz=2000 --seed=3 --fuzz-strategy=mcmc --fuzz-mcmc-share=0.5 | FileCheck %s --check-prefix=SHARE
//
// the cost of the chain on fuzz.progress
// RUN: sea-run %s --program=safe --fuzz-time=0.25 --fuzz-progress=0.1 --seed=1 --fuzz-strategy=mcmc | FileCheck %s --check-prefix=PROGRESS
// RUN: sea-run %s --program=safe --fuzz=2000 --seed=1 --fuzz-strategy=mcmc --fuzz-gates > %t.txt
// RUN: sea-run %s --program=safe --fuzz=2000 --seed=1 --fuzz-strategy=mcmc --fuzz-gates --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl --require stat:moves,accepted,stall,beta
//
// RUN: not sea-run %s --program=safe --fuzz=10 --fuzz-mcmc-beta=2 2>&1 | FileCheck %s --check-prefix=NOSTRAT
// RUN: not sea-run %s --program=safe --fuzz-strategy=mcmc 2>&1 | FileCheck %s --check-prefix=NOFUZZ
// RUN: not sea-run %s --program=safe --fuzz=10 --fuzz-strategy=mcmc --fuzz-cmp=false 2>&1 | FileCheck %s --check-prefix=NOCMP
// RUN: not sea-run %s --program=safe --fuzz=10 --fuzz-strategy=mcmc --fuzz-mcmc-beta=-1 2>&1 | FileCheck %s --check-prefix=BETA
// RUN: not sea-run %s --program=safe --fuzz=10 --fuzz-strategy=mcmc --fuzz-mcmc-share=2 2>&1 | FileCheck %s --check-prefix=SHAREBAD

// MCMC: fuzz.failure: 1 trial={{[0-9]+}} at={{.*}}mcmc.mlir:[[@LINE+36]]:3
// the failing trial is the chain's best state: cost 0
// MCMC: fuzz.mcmc: moves={{[1-9][0-9]*}} accepted={{[1-9][0-9]*}} uphill={{[0-9]+}} restarts={{[0-9]+}} failure-restarts=1 best-cost=0.00 best-trial={{[1-9][0-9]*}} stall=0:{{[1-9][0-9]*}} beta=1 restart=1000 share=1
// MCMC: fuzz.gate: 0 op=sea.assume passed=1 {{.*}} mcmc-moves={{[1-9][0-9]*}} mcmc-improved={{[1-9][0-9]*}} check=1
// FILE: gate: 0 op=sea.assume passed=1 blocked={{[0-9]+}} distance={{[0-9]+}} mcmc-moves={{[1-9][0-9]*}} mcmc-improved={{[1-9][0-9]*}}{{$}}

// CORPUS-NOT: fuzz.failure:
// CORPUS-NOT: fuzz.mcmc
// CORPUS: fuzz.gate-closed: 0 blocked=20000
// CORPUS-NOT: mcmc-moves
// FILE0: gate: 0 op=sea.assume passed=0 blocked=20000 distance={{[0-9]+}}{{$}}

// RESTART: fuzz.mcmc: moves={{[0-9]+}} {{.*}} restarts={{[1-9][0-9]*}} {{.*}} restart=1 share=1
// NORESTART: fuzz.mcmc: moves=299 {{.*}} restarts=0 failure-restarts=0 {{.*}} restart=0 share=1

// SHARE0: fuzz.mcmc: moves=0 accepted=0 uphill=0 restarts=0 failure-restarts=0 best-cost=- stall=- beta=1 restart=1000 share=0
// SHARE: fuzz.mcmc: moves={{[1-9][0-9]*}} {{.*}} share=0.5

// PROGRESS: fuzz.progress: {{.*}} corpus={{[0-9]+}} cost={{[0-9]+\.[0-9][0-9]}}{{$}}
// PROGRESS: fuzz.mcmc:

// NOSTRAT: need --fuzz-strategy=mcmc
// NOFUZZ: need --fuzz or --fuzz-time
// NOCMP: --fuzz-strategy=mcmc needs the branch distances
// BETA: --fuzz-mcmc-beta must not be negative
// SHAREBAD: --fuzz-mcmc-share must be in [0, 1]
sea.program @narrow {
  %x = sea.nondet "nondet_u32" [0] : i32
  %c3 = sea.const 3 : i32
  %c7 = sea.const 7 : i32
  %m = sea.mul %x, %c3 : i32
  %y = sea.add %m, %c7 : i32
  %k = sea.const 1000003 : i32
  %eq = sea.icmp eq, %y, %k : i32
  sea.assume %eq
  %f = sea.const false
  sea.assert %f
  sea.halt
}

sea.program @safe {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assume %lt
  %y = sea.nondet "nondet_u8" [1] : i8
  %c5 = sea.const 5 : i8
  %small = sea.icmp ult, %y, %c5 : i8
  sea.cond_br %small, ^a, ^b
^a:
  sea.br ^done
^b:
  sea.br ^done
^done:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
