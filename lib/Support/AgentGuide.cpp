// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- AgentGuide.cpp - --agent guides and the docs root ------------------===//

#include "sea/Support/AgentGuide.h"
#include "sea/Support/InstallLayout.h"
#include "sea/Support/Version.h"

#include "llvm/Support/FileSystem.h"

using namespace llvm;

namespace sea {

static bool hasDocs(StringRef root) {
  return sys::fs::is_directory(joinClean(root, "docs"));
}

DocsRoot findDocsRoot(StringRef exePath) {
  DocsRoot d;
  if (exePath.empty())
    return d;
  // an install: <prefix>/share/doc/sea-mlir/docs
  std::string inst = installShareDir(exePath, "doc/sea-mlir");
  if (hasDocs(inst)) {
    d.kind = DocsRoot::Install;
    d.root = inst;
    return d;
  }
  // a build directory: the source tree its CMakeCache.txt names
  std::string src = buildSourceTree(exePath);
  if (!src.empty() && hasDocs(src)) {
    d.kind = DocsRoot::SourceTree;
    d.root = src;
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
     << kDocsWebSite
     << "<page>/ except docs/design/ and docs/<Capitalized>.md, which are "
        "only in the repository: "
     << kDocsRepoTree << docsRepoRef() << "/docs)\n";
}

std::string docsRepoRef() {
  std::string tag = ("v" + versionNumber()).str();
  return gitDescribe() == tag ? tag : "main";
}

void printAgentGuide(raw_ostream &os, StringRef guide, const char *argv0) {
  auto [title, rest] = guide.split('\n');
  os << title << '\n';
  printDocsLines(os, findDocsRoot(mainExecutableRealPath(argv0)));
  os << rest;
}

} // namespace sea
