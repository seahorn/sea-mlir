// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- InstallLayout.h - data files found from the executable ---*- C++ -*-===//
//
// The tools find their data files (docs, sea-vc profiles) at run time from
// the executable's real path; no source or build path is compiled in. Two
// layouts:
//   install:   <prefix>/bin/<tool>, data under <prefix>/share/...
//              (CMakeLists.txt install rules; symlinks resolved first, so a
//              Homebrew bin/ link finds its prefix)
//   build dir: <build>/bin/<tool>; <build>/CMakeCache.txt names the source
//              tree (`sea-mlir_SOURCE_DIR`), read when asked
// test/Tools/agent-docs.test, test/VC/config-lookup.test and
// test/Tools/no-source-paths.test pin them.
//
//===----------------------------------------------------------------------===//
#ifndef SEA_SUPPORT_INSTALLLAYOUT_H
#define SEA_SUPPORT_INSTALLLAYOUT_H

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace sea {
/// The real path (symlinks resolved) of the running executable; \p argv0
/// is the fallback where the OS does not name it. Empty if unknown.
std::string mainExecutableRealPath(const char *argv0);

/// `<dir>/<parts...>` without `.` / `..` components.
llvm::SmallString<256> joinClean(llvm::StringRef dir, llvm::StringRef a,
                                 llvm::StringRef b = "", llvm::StringRef c = "",
                                 llvm::StringRef d = "");

/// `<exe dir>/../share/<sub>` for the executable at \p exePath (a real
/// path): where an install keeps its data. Not checked for existence.
std::string installShareDir(llvm::StringRef exePath, llvm::StringRef sub);

/// The source tree of the build directory holding \p exePath (a real path)
/// as <build>/bin/<tool>: `sea-mlir_SOURCE_DIR` of <build>/CMakeCache.txt.
/// Empty if there is no such file or entry.
std::string buildSourceTree(llvm::StringRef exePath);
} // namespace sea

#endif // SEA_SUPPORT_INSTALLLAYOUT_H
