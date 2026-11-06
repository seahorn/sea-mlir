// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Version.h - the sea-mlir version -------------------------*- C++ -*-===//
//
// The release version (project(VERSION) in CMakeLists.txt), `git describe`
// of the source tree at build time (cmake/SeaVersion.cmake; "unknown"
// outside a git checkout, or -DSEA_GIT_DESCRIBE=<text>), and the LLVM
// revision the tree is pinned to (LLVM_REVISION). Every tool prints them
// with --version; sea-vc also prints `tool.version:` first (docs/Release.md).
//
//===----------------------------------------------------------------------===//
#ifndef SEA_SUPPORT_VERSION_H
#define SEA_SUPPORT_VERSION_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace sea {
/// "0.1.0"
llvm::StringRef versionNumber();
/// `git describe --tags --always --dirty` at build time, or "unknown"
llvm::StringRef gitDescribe();
/// the pinned LLVM revision (LLVM_REVISION)
llvm::StringRef llvmRevision();
/// "sea-mlir 0.1.0 (<describe>)": one token-free line for logs
std::string versionString();
/// Installs a --version printer for `tool`: the version string, the LLVM
/// revision and the build type, then `extra` lines (solver versions).
void setVersionPrinter(llvm::StringRef tool,
                       void (*extra)(llvm::raw_ostream &) = nullptr);
} // namespace sea

#endif // SEA_SUPPORT_VERSION_H
