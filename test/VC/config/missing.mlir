// A config that is not found is an error: a bare name lists the searched
// directories, a path says there is no such file.
// RUN: env SEA_MLIR_PROFILE_PATH=%S/Inputs/profiles not sea-vc %s --config=no-such-profile 2>&1 | FileCheck %s --check-prefix=NAME
// RUN: not sea-vc %s --config=%S/Inputs/no-such.cfg 2>&1 | FileCheck %s --check-prefix=PATH
// NAME: no-such-profile.cfg in {{.*}}Inputs/profiles, {{.*}}profiles [vc.config-not-found]
// PATH: no such file [vc.config-not-found]
sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %y = sea.add %x, %c100 : i8
  %ok = sea.icmp uge, %y, %c100 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
