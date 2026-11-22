# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""A program whose global is preloaded by a 4 KiB sea.mem.fill."""

data = [(i * 37 + 11) % 256 for i in range(4096)]
print(f"""!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @big {{
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 4096 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<{data}> : tensor<4096xi8> inplace : !m
  %i = sea.nondet "nd" [1] : i64
  %c = sea.const 4088 : i64
  %j = sea.and %i, %c : i64
  %q = sea.ptr.add %g, %j : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] align 8 : !m -> i64
  %z = sea.const 0 : i64
  %ok = sea.icmp ne, %x, %z : i64
  sea.assert %ok
  sea.halt
}}""")
