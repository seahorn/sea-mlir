// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Version.cpp - the sea-mlir version ---------------------------------===//

#include "sea/Support/Version.h"
#include "sea/Support/VersionInfo.inc"

#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"

#include <string>

using namespace llvm;

StringRef sea::versionNumber() { return SEA_MLIR_VERSION; }
StringRef sea::gitDescribe() { return SEA_MLIR_GIT_DESCRIBE; }
StringRef sea::llvmRevision() { return SEA_MLIR_LLVM_REVISION; }

std::string sea::versionString() {
  return (Twine("sea-mlir ") + versionNumber() + " (" + gitDescribe() + ")")
      .str();
}

static std::string gTool;
static void (*gExtra)(raw_ostream &) = nullptr;

void sea::setVersionPrinter(StringRef tool, void (*extra)(raw_ostream &)) {
  gTool = tool.str();
  gExtra = extra;
  cl::SetVersionPrinter([](raw_ostream &os) {
    os << gTool << " (" << versionString() << ")\n"
       << "  LLVM/MLIR " << LLVM_VERSION_STRING << " at " << llvmRevision()
       << "\n"
       << "  build: " << SEA_BUILD_TYPE
#ifndef NDEBUG
       << ", assertions on"
#endif
       << "\n";
    if (gExtra)
      gExtra(os);
  });
}
