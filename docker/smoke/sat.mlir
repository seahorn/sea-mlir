// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
sea.program @sat {
  %x = sea.nondet "value" [0] : i8
  %zero = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %zero : i8
  sea.assert %ok
  sea.halt
}
