// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- AgentGuide.cpp - --agent guides and the docs root ------------------===//

#include "sea/Support/AgentGuide.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace llvm;

namespace sea {

/// `<dir>/<parts...>` without `.` / `..` components.
static SmallString<256> joinClean(StringRef dir, StringRef a, StringRef b = "",
                                  StringRef c = "", StringRef d = "") {
  SmallString<256> p(dir);
  sys::path::append(p, a, b, c, d);
  sys::path::remove_dots(p, /*remove_dot_dot=*/true);
  return p;
}

static bool hasDocs(StringRef root) {
  return sys::fs::is_directory(joinClean(root, "docs"));
}

DocsRoot findDocsRoot(StringRef exePath) {
  DocsRoot d;
  if (exePath.empty())
    return d;
  SmallString<256> real;
  if (sys::fs::real_path(exePath, real))
    real = exePath;
  StringRef exeDir = sys::path::parent_path(real);
  // an install: <prefix>/bin/<tool>, <prefix>/share/doc/sea-mlir/docs
  SmallString<256> inst = joinClean(exeDir, "..", "share", "doc", "sea-mlir");
  if (hasDocs(inst)) {
    d.kind = DocsRoot::Install;
    d.root = std::string(inst);
    return d;
  }
  // a build directory: <build>/bin/<tool>, <build>/CMakeCache.txt names
  // the source tree
  auto buf = MemoryBuffer::getFile(joinClean(exeDir, "..", "CMakeCache.txt"),
                                   /*IsText=*/true);
  if (!buf)
    return d;
  SmallVector<StringRef> lines;
  (*buf)->getBuffer().split(lines, '\n');
  for (StringRef line : lines) {
    line = line.rtrim("\r");
    if (!line.consume_front("sea-mlir_SOURCE_DIR:"))
      continue;
    StringRef src = line.split('=').second;
    if (!src.empty() && hasDocs(src)) {
      d.kind = DocsRoot::SourceTree;
      d.root = src.str();
    }
    break;
  }
  return d;
}

void printDocsLines(raw_ostream &os, const DocsRoot &d) {
  switch (d.kind) {
  case DocsRoot::Install:
    os << "DOCS: " << d.root
       << " (installed with this binary: the docs of this release; the "
          "docs/... paths in this guide are relative to it)\n";
    break;
  case DocsRoot::SourceTree:
    os << "DOCS: " << d.root
       << " (the source tree of this build directory: may be newer than "
          "this binary; the docs/... paths in this guide are relative to "
          "it)\n";
    break;
  case DocsRoot::None:
    os << "DOCS: none (no share/doc/sea-mlir/docs next to this binary and "
          "no source tree; use DOCS-WEB)\n";
    break;
  }
  os << "DOCS-WEB: " << kDocsWebSite
     << " (follows main, not this release; docs/<page>.md is at "
     << kDocsWebSite << "<page>/)\n";
}

void printAgentGuide(raw_ostream &os, StringRef guide, const char *argv0) {
  auto [title, rest] = guide.split('\n');
  os << title << '\n';
  std::string exe = sys::fs::getMainExecutable(
      argv0, reinterpret_cast<void *>(&printAgentGuide));
  printDocsLines(os, findDocsRoot(exe));
  os << rest;
}

} // namespace sea
