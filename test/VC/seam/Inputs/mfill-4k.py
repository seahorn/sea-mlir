# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Two 4 KiB globals preloaded by sea.mem.fill (one with varied bytes, one
zero), read at decided offsets and, the zero one, at a symbolic offset."""

data = [(i * 37 + 11) % 256 for i in range(4096)]
word = sum(data[1000 + b] << (8 * b) for b in range(8))
print(f"""!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @big {{
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 4096 align 8 : !sea.ptr<64>
  %z = sea.global @zero size 4096 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<{data}> : tensor<4096xi8> inplace : !m
  %m2 = sea.mem.fill %m1[%z], dense<0> : tensor<4096xi8> inplace : !m
  %c1000 = sea.const 1000 : i64
  %q = sea.ptr.add %g, %c1000 : !sea.ptr<64>, i64
  %x = sea.load %m2[%q] align 8 : !m -> i64
  %xv = sea.const {word} : i64
  %ok1 = sea.icmp eq, %x, %xv : i64
  %c4095 = sea.const 4095 : i64
  %r = sea.ptr.add %g, %c4095 : !sea.ptr<64>, i64
  %y = sea.load %m2[%r] : !m -> i8
  %yv = sea.const {data[4095]} : i8
  %ok2 = sea.icmp eq, %y, %yv : i8
  %i = sea.nondet "nd" [1] : i64
  %c4096 = sea.const 4096 : i64
  %lt = sea.icmp ult, %i, %c4096 : i64
  sea.assume %lt
  %s = sea.ptr.add %z, %i : !sea.ptr<64>, i64
  %w = sea.load %m2[%s] : !m -> i8
  %c0 = sea.const 0 : i8
  %ok3 = sea.icmp eq, %w, %c0 : i8
  %ok12 = sea.and %ok1, %ok2 : i1
  %ok = sea.and %ok12, %ok3 : i1
  sea.assert %ok
  sea.halt
}}""")
