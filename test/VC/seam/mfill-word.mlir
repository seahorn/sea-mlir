// A fill outside the scope of the mfill term (docs/design/MemFill.md):
// under --seam the VC refuses the fill of a word-layout or keyed memory
// without the lowering (vc.mem-fill); with --lower sea-expand-mem-fill
// expands it and counts why (expanded-word-layout, expanded-keyed).
// RUN: not sea-vc %s --seam --program=w 2>&1 | FileCheck %s --check-prefix=REFUSE
// RUN: not sea-vc %s --seam --program=k 2>&1 | FileCheck %s --check-prefix=REFUSE
// RUN: sea-vc %s --lower --seam --program=w 2>&1 | FileCheck %s --check-prefix=WORD
// RUN: sea-vc %s --lower --seam --program=k 2>&1 | FileCheck %s --check-prefix=KEYED
// REQUIRES: z3
// REFUSE: sea.mem.fill of a memory that is not a byte memory (word layout, keyed){{.*}}[vc.mem-fill]
// WORD: vc.expand-mem-fill: expanded=1 cells=2 kept=0 expanded-word-layout=1 expanded-keyed=0 expanded-other=0
// WORD: result: unsat
// KEYED: vc.expand-mem-fill: expanded=1 cells=2 kept=0 expanded-word-layout=0 expanded-keyed=1 expanded-other=0
// KEYED: result: unsat

!k = !sea.mem<!sea.addr<8>, i8, keyed>
!w = !sea.mem<!sea.addr<8>, i32, aligned>
sea.program @w {
  %c16 = sea.const 16 : i8
  %s = sea.addr.from_int %c16 : i8 to !sea.addr<8>
  %m0 = sea.mem.nondet "mem" [0] : !w
  %m1 = sea.mem.fill %m0[%s], dense<[1, 2]> : tensor<2xi32> inplace : !w
  %x = sea.load %m1[%s] : !w -> i32
  %c1 = sea.const 1 : i32
  %ok = sea.icmp eq, %x, %c1 : i32
  sea.assert %ok
  sea.halt
}
sea.program @k {
  %c16 = sea.const 16 : i8
  %s = sea.addr.from_int %c16 : i8 to !sea.addr<8>
  %m0 = sea.mem.nondet "mem" [0] : !k
  %m1 = sea.mem.fill %m0[%s], dense<[1, 2]> : tensor<2xi8> inplace : !k
  %x = sea.load %m1[%s] : !k -> i8
  %c1 = sea.const 1 : i8
  %ok = sea.icmp eq, %x, %c1 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
