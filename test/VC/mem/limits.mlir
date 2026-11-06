// RUN: not sea-vc %s --program=memset 2>&1 | FileCheck %s --check-prefix=REFUSE
// RUN: not sea-vc %s --program=clobbered | FileCheck %s --check-prefix=STALE
// RUN: not sea-vc %s --program=not_inplace | FileCheck %s --check-prefix=NOTIP

!m8 = !sea.mem<!sea.addr<8>, i8>

// REFUSE: Error: sea.memset with a symbolic length: the array encoding cannot express it
// REFUSE-SAME: [vc.mem-op-symbolic-length]
sea.program @memset {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %n = sea.nondet "nd_len" [1] : i8
  %m1 = sea.memset %m0[%a], %z, %n inplace : !m8
  %x = sea.load %m1[%a] : !m8 -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// A load of an earlier version whose cell was clobbered: the VC reads the
// old value, the in-place interpreter the new one; replay reports it.
// STALE: result: sat
// STALE: mismatch: sea.load
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

// The VC accepts persistent updates; replay cannot run them.
// NOTIP: result: sat
// NOTIP: replay: error
// NOTIP: message: memory update without inplace
sea.program @not_inplace {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%a], %v : !m8, i8
  %x = sea.load %m1[%a] : !m8 -> i8
  %ok = sea.icmp ne, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
