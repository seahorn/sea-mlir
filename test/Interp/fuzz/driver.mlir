// utils/fuzz-driver.py drives sea-run --fuzz-serve with a strategy that
// samples site 1 in [0,99] and site 0 in [0, site 1]: every run passes both
// assumes, and it finds the failing assert (y - x == 37, behind an xor).
// The failure file replays with --input-file.
// RUN: rm -f %t.txt
// RUN: %python %sea_src_root/utils/fuzz-driver.py %s --sea-run %sea_bin/sea-run --sea-run-arg=--program=main --requests 2000 --seed 1 --out %t.txt --strategy '{"sites": {"1": {"range": [0, 99]}, "0": {"range": [0, "s1"]}}}' | FileCheck %s
// RUN: sea-run %s --program=main --input-file=%t.txt | FileCheck %s --check-prefix=REPLAY
// A strict relation needs arithmetic on a bound: site 0 in [0, s1 - 1].
// Every run passes both assumes (none is blocked); y = 0 makes the range
// empty, and the sample is drawn again.
// RUN: %python %sea_src_root/utils/fuzz-driver.py %s --sea-run %sea_bin/sea-run --sea-run-arg=--program=strict --requests 2000 --seed 1 --out %t.strict.txt --strategy '{"sites": {"0": {"range": [0, "s1 - 1"]}, "1": {"range": [0, 99]}}}' | FileCheck %s --check-prefix=STRICT
// RUN: sea-run %s --program=strict --input-file=%t.strict.txt | FileCheck %s --check-prefix=REPLAY
// Bad strategies: a bound naming a site not listed, a malformed bound.
// RUN: not %python %sea_src_root/utils/fuzz-driver.py %s --sea-run %sea_bin/sea-run --strategy '{"sites": {"0": {"range": [0, "s1"]}}}' 2>&1 | FileCheck %s --check-prefix=UNLISTED
// RUN: not %python %sea_src_root/utils/fuzz-driver.py %s --sea-run %sea_bin/sea-run --strategy '{"sites": {"1": {"range": [0, 9]}, "0": {"range": [0, "s1 * 2"]}}}' 2>&1 | FileCheck %s --check-prefix=MALFORMED

// CHECK: requests: {{[0-9]+}}
// CHECK: status: {{.*}}assert-failed=1
// CHECK: failure: assert-failed file=
// REPLAY: status: assert-failed
// STRICT: status: {{(halted=[0-9]+ )?}}assert-failed=1{{$}}
// STRICT: failure: assert-failed
// UNLISTED: a bound names site 1, which the strategy does not list
// MALFORMED: bad bound 's1 * 2'

sea.program @main {
  %x = sea.nondet "nd_size_t" [0] : i64
  %y = sea.nondet "nd_size_t" [1] : i64
  %c100 = sea.const 100 : i64
  %lt = sea.icmp ult, %y, %c100 : i64
  sea.assume %lt
  %le = sea.icmp ule, %x, %y : i64
  sea.assume %le
  %d = sea.sub %y, %x : i64
  %k = sea.const 37 : i64
  %t = sea.xor %d, %k : i64
  %z = sea.const 0 : i64
  %ok = sea.icmp ne, %t, %z : i64
  sea.assert %ok
  sea.halt
}

sea.program @strict {
  %x = sea.nondet "nd_size_t" [0] : i64
  %y = sea.nondet "nd_size_t" [1] : i64
  %c100 = sea.const 100 : i64
  %lt = sea.icmp ult, %y, %c100 : i64
  sea.assume %lt
  %ltx = sea.icmp ult, %x, %y : i64
  sea.assume %ltx
  %d = sea.sub %y, %x : i64
  %k = sea.const 37 : i64
  %t = sea.xor %d, %k : i64
  %z = sea.const 0 : i64
  %ok = sea.icmp ne, %t, %z : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
