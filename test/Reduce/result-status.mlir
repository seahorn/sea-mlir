// utils/reduce-test.py --mode result with an expected exit status: a sat
// whose replay disagrees with the model exits 2 with an `Error:` line and
// `result: unknown`; --status 2 makes that run the symptom (interesting,
// exit 1) instead of a sea-vc error. Controls (exit 0): another status, or
// the result sea-vc does not print.
// RUN: not %python %sea_src_root/utils/reduce-test.py test --mode result --config= --result unknown --status 2 --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode result --config= --result unknown --status 0 --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode result --config= --result sat --status 2 --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
!m8 = !sea.mem<!sea.addr<8>, i8>
sea.program @clobbered {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%a], %v inplace : !m8, i8
  %old = sea.load %m0[%a] : !m8 -> i8
  %ok = sea.icmp eq, %old, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
