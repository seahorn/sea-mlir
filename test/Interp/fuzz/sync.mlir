// --fuzz-sync: every --fuzz-sync-period seconds the run reads the cov-*.txt
// files of the given directories (other processes' --fuzz-out) that it has
// not read yet and runs each as a trial; one that adds coverage joins the
// corpus, but is not written to this run's --fuzz-out. Files that do not
// parse, or hold a value that does not fit its site, are skipped and
// counted; other names (and the run's own --fuzz-out) are not read.
// RUN: rm -rf %t && mkdir -p %t/peer
// The magic value is x ^ 0x5a5a5a5a == 0x12345678: random draws miss it.
// RUN: printf '# found elsewhere\n0=1215171618\n' > %t/peer/cov-7.txt
// RUN: printf 'bogus\n' > %t/peer/cov-8.txt
// RUN: printf '0=99999999999\n' > %t/peer/cov-9.txt
// RUN: printf '0=1215171618\n' > %t/peer/failure-3.txt
// RUN: printf '0=1215171618\n' > %t/peer/.cov-10.txt.tmp
// RUN: sea-run %s --fuzz-time=1 --seed=1 --fuzz-out=%t/own --fuzz-sync=%t/peer,%t/own,%t/nosuch --fuzz-sync-period=0.1 | FileCheck %s --check-prefix=SYNC
// RUN: not grep -rl 1215171618 %t/own
// One directory under three spellings is imported once; a dotfile in a
// corpus directory is not read; a skipped file is also a fuzz.sync-skip
// warning (once per kind).
// RUN: sea-run %s --fuzz-time=1 --seed=1 --fuzz-sync=%t/peer,%t/peer/,%t/./peer --fuzz-sync-period=0.1 2>&1 | FileCheck %s --check-prefix=SPELL
// RUN: mkdir -p %t/cdir && printf '0=3\n' > %t/cdir/a.txt && printf 'bogus\n' > %t/cdir/.hidden
// RUN: sea-run %s --fuzz=20 --seed=1 --corpus=%t/cdir | FileCheck %s --check-prefix=DOT
// Without sync the block stays uncovered.
// RUN: sea-run %s --fuzz-time=0.3 --seed=1 | FileCheck %s --check-prefix=NOSYNC
// RUN: not sea-run %s --fuzz=10 --fuzz-sync=%t/peer --fuzz-sync-period=0 2>&1 | FileCheck %s --check-prefix=PERIOD

// SYNC: fuzz.coverage: blocks=3 blocks-total=3
// SYNC: fuzz.sync: imported=1 kept=1 skipped=2 polls={{[1-9][0-9]*}} dirs=3 missing=1 period=0.1
// SPELL: fuzz.sync-skip
// SPELL: fuzz.sync: imported=1 kept=1 skipped=2
// DOT: fuzz.corpus: {{.*}} seeds=1
// NOSYNC: fuzz.coverage: blocks=2 blocks-total=3
// NOSYNC-NOT: fuzz.sync:
// PERIOD: --fuzz-sync-period must be positive

sea.program @main {
  %x = sea.nondet "nondet_u32" [0] : i32
  %k = sea.const 1515870810 : i32
  %y = sea.xor %x, %k : i32
  %c = sea.const 305419896 : i32
  %eq = sea.icmp eq, %y, %c : i32
  sea.cond_br %eq, ^rare, ^done
^rare:
  sea.br ^done
^done:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
