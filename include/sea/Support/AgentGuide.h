// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- AgentGuide.h - --agent guides and the docs root ----------*- C++ -*-===//
//
// Every tool's --agent guide cites the long docs as repo-relative paths
// (`docs/agents/slow-vc.md`). printAgentGuide prints the guide with two
// lines after its title that say where `docs/` is, found at run time from
// the executable's real path (symlinks resolved: Homebrew links bin/ into
// the prefix):
//
//   DOCS: <root> (...)       <root>/docs/... is a cited path
//   DOCS-WEB: <site> (...)   the web site, which follows main, and the
//                            repository docs of this release (docsRepoRef)
//
// <root> is, in order: an install, `<exe dir>/../share/doc/sea-mlir`
// when it has `docs/` (the install rules of CMakeLists.txt; the docs of
// this binary's release); a build directory, the source tree named by
// `<exe dir>/../CMakeCache.txt` (`sea-mlir_SOURCE_DIR`) when it has
// `docs/` (read at run time: no path is compiled in); else none. utils/
// reduce-test.py prints the same lines (sea-reduce-test), with the ref of
// the sea-opt it runs (`--version`); test/Tools/agent-docs.test compares.
//
//===----------------------------------------------------------------------===//
#ifndef SEA_SUPPORT_AGENTGUIDE_H
#define SEA_SUPPORT_AGENTGUIDE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace sea {
/// The site of the docs (mkdocs.yml site_url).
inline constexpr llvm::StringLiteral kDocsWebSite =
    "https://seahorn.github.io/sea-mlir/";

/// The repository's tree view; + ref + "/docs" is the docs of that ref
/// (mkdocs.yml excludes docs/design/ and docs/<Capitalized>.md from the
/// site).
inline constexpr llvm::StringLiteral kDocsRepoTree =
    "https://github.com/seahorn/sea-mlir/tree/";

/// The ref of the repository docs that match this binary: the release tag
/// `v<version>` when `git describe` at build time is exactly that tag (a
/// clean build of the tag, or -DSEA_GIT_DESCRIBE=v<version>), else "main".
std::string docsRepoRef();

/// Where the docs of a binary are.
struct DocsRoot {
  enum Kind { Install, SourceTree, None };
  Kind kind = None;
  /// the directory holding `docs/` (empty for None)
  std::string root;
};

/// The docs root of the executable at \p exePath (a real path; see the
/// file comment and sea/Support/InstallLayout.h).
DocsRoot findDocsRoot(llvm::StringRef exePath);

/// The DOCS and DOCS-WEB lines for \p d, each ending in a newline.
void printDocsLines(llvm::raw_ostream &os, const DocsRoot &d);

/// Prints \p guide: its first (title) line, the DOCS lines of the running
/// executable (\p argv0: fallback when the OS does not name it), then the
/// rest of the guide.
void printAgentGuide(llvm::raw_ostream &os, llvm::StringRef guide,
                     const char *argv0);
} // namespace sea

#endif // SEA_SUPPORT_AGENTGUIDE_H
