// Where a bare --config name is looked up, found at run time from sea-vc's
// real path (sea/Support/InstallLayout.h): after $SEA_MLIR_PROFILE_PATH,
// <exe dir>/../share/sea-mlir/profiles, then, in a build directory, the
// source tree that <build>/CMakeCache.txt names. No path is compiled in.

// A build directory: the install-layout dir, then the source tree's.
// RUN: env -u SEA_MLIR_PROFILE_PATH not sea-vc %s --config=no-such-profile 2>&1 | FileCheck %s -DSRC=%sea_src_root --check-prefix=BUILD
// BUILD: no-such-profile.cfg in {{[^,]*}}/share/sea-mlir/profiles, [[SRC]]/profiles [vc.config-not-found]

// An install reached through a symlink: its share/sea-mlir/profiles, and
// not the source tree of the build that made it.
// RUN: rm -rf %t && mkdir -p %t/prefix/bin %t/prefix/share/sea-mlir/profiles %t/brew/bin
// RUN: ln %sea_bin/sea-vc %t/prefix/bin/sea-vc || cp %sea_bin/sea-vc %t/prefix/bin/sea-vc
// RUN: ln -s %t/prefix/bin/sea-vc %t/brew/bin/sea-vc
// RUN: cp %S/Inputs/profiles/stats.cfg %t/prefix/share/sea-mlir/profiles/mine.cfg
// RUN: env -u SEA_MLIR_PROFILE_PATH %t/brew/bin/sea-vc %s --config=mine | FileCheck %s --check-prefix=INSTALL
// INSTALL: vc.config: {{.*}}/prefix/share/sea-mlir/profiles/mine.cfg
// INSTALL: result: sat
// RUN: env -u SEA_MLIR_PROFILE_PATH not %t/brew/bin/sea-vc %s --config=seam 2>&1 | FileCheck %s --check-prefix=NOSRC
// NOSRC: seam.cfg in {{[^,]*}}/prefix/share/sea-mlir/profiles [vc.config-not-found]
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
