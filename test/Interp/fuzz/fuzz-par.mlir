// utils/fuzz-par.py: N sea-run --fuzz processes with distinct seeds, their
// own --fuzz-out and coverage files; per-process lines, the first failure,
// the union of the coverage files.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %python %sea_src_root/utils/fuzz-par.py -n 2 --out %t/safe --sea-run %sea_bin/sea-run -- %s --program=safe --fuzz=2000 | FileCheck %s --check-prefix=SAFE
// RUN: %python %sea_src_root/utils/fuzz-par.py -n 2 --out %t/fail --wait-all --sea-run %sea_bin/sea-run -- %s --program=fail --fuzz=100000 | FileCheck %s --check-prefix=FAIL
// RUN: %python %sea_src_root/utils/fuzz-par.py -n 2 --out %t/prog --progress 0.1 --sea-run %sea_bin/sea-run -- %s --program=safe --fuzz-time=0.5 | FileCheck %s --check-prefix=PROG
// RUN: not %python %sea_src_root/utils/fuzz-par.py -n 2 --out %t/bad --sea-run %sea_bin/sea-run -- %s --fuzz=10 --seed=3 2>&1 | FileCheck %s --check-prefix=RESERVED
// RUN: not %python %sea_src_root/utils/fuzz-par.py -n 2 --out %t/bad1 --sea-run %sea_bin/sea-run -- %s --fuzz=10 -seed=3 2>&1 | FileCheck %s --check-prefix=SINGLE
// RUN: not %python %sea_src_root/utils/fuzz-par.py -n 2 --out %t/err --sea-run %sea_bin/sea-run -- %s --program=nosuch --fuzz=10 | FileCheck %s --check-prefix=ERR
// RUN: not %python %sea_src_root/utils/fuzz-par.py -n 2 --out %t/safe --sea-run %sea_bin/sea-run -- %s --fuzz=10 2>&1 | FileCheck %s --check-prefix=NONEMPTY
// RUN: %python %sea_src_root/utils/fuzz-par.py --agent | FileCheck %s --check-prefix=AGENT

// The union joins the coverage files of the processes that ended.
// SAFE-DAG: fuzz-par.process: 0 seed=1 exit=0 end=done from=final trials=2000
// SAFE-DAG: fuzz-par.process: 1 seed=2 exit=0 end=done from=final trials=2000
// SAFE: fuzz-par.first-failure: none
// SAFE: fuzz-par.union: blocks=4 blocks-total=4 edges=5 assumes=1 assumes-total=1 paths=3 processes=2/2

// FAIL: fuzz-par.process: 0 seed=1 exit=0 end=failure from=final
// FAIL: fuzz-par.process: 1 seed=2 exit=0 end=failure from=final
// FAIL: fuzz-par.first-failure: process={{[01]}} seed={{[12]}} kind=failure trial={{[0-9]+}} time={{[0-9.]+}} wall={{[0-9.]+}} at={{.*}} file={{.*}}failure-{{[0-9]+}}.txt
// FAIL: fuzz-par.summary: processes=2 wall={{[0-9.]+}} failures=2

// PROG: fuzz-par.progress: {{[0-9.]+}} reported=2/2 trials-sum={{[1-9][0-9]*}} steps-sum={{[1-9][0-9]*}} failures-sum=0 blocks-max=4 edges-max=5 paths-max=3 assumes-max=1
// PROG: fuzz-par.union: {{.*}} processes=2/2

// SINGLE: -seed=3: the launcher sets
// ERR: fuzz-par.error: process=
// ERR: fuzz-par.process: 0 seed=1 exit=1 end=error
// RESERVED: --seed=3: the launcher sets
// NONEMPTY: is not a new or empty directory
// AGENT: fuzz-par: run N `sea-run --fuzz` processes
sea.program @safe {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assume %lt
  %c5 = sea.const 5 : i8
  %small = sea.icmp ult, %x, %c5 : i8
  sea.cond_br %small, ^a, ^b
^a:
  sea.br ^done
^b:
  sea.br ^done
^done:
  sea.halt
}

sea.program @fail {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assert %lt
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
