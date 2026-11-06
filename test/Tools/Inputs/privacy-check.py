#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exercise the scanner with generated disclosure fixtures and a scratch tree."""

import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location("privacy_check", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

# Construct fixture data so the publication scan does not flag this test itself.
person = "Example Person"
local = "/" + "home" + "/developer/project"
examples = [
    local,
    "/" + "ag" + "/project",
    "/" + "tmp" + "/scratch",
    "journal" + "/entry.md",
    "note" + "book",
    "review " + "31",
    "R" + "23",
    "round " + "3",
    "2026" + "-11-02",
    "plan " + "B.1",
    "plan " + "B2",
    "plan " + "2.3",
    "plan " + "step",
    "phase " + "4",
]
for value in examples:
    assert list(module.findings("docs/example.md", value)), value

terms = [module.re.compile("Example Person"), module.re.compile("examplehost")]
vocabulary = "# BEGIN DETECTION VOCABULARY\nexamplehost\n# END DETECTION VOCABULARY"
assert list(module.findings("utils/privacy-check.py", vocabulary, terms))
credits = "## 🐬 Built with AI agents\n" + person + "\n"
assert not list(module.findings("README.md", credits, terms))
assert list(module.findings("README.md", credits + "## Usage\n" + person, terms))
assert list(module.findings("README.md", credits + local, terms))
assert not list(module.findings("AUTHORS", person, terms))
assert list(module.findings("AUTHORS", local, terms))
assert not list(module.findings("docs/reference/seam.md", "> " + person, terms))
assert list(module.findings("docs/reference/seam.md", person, terms))
assert list(module.findings("docs/reference/seam.md", "> " + local, terms))
assert not list(module.findings("code.cpp", "private: // libraries; hasTrait; r23"))
assert not list(module.findings("LLVM_REVISION", "date: " + "2026" + "-10-30"))
assert list(module.findings("docs/x.md", "date: " + "2026" + "-10-30"))
assert not list(module.findings("test.test", "%t/home/results; CLAUDE.md"))

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    subprocess.run(["git", "init", "-q", directory], check=True)
    (root / "tracked.md").write_text("safe\n")
    (root / ".gitignore").write_text("generated/\n.privacy-terms\n")
    subprocess.run(["git", "-C", directory, "add", "."], check=True)
    (root / "generated").mkdir()
    (root / "generated" / "log").write_text(local)

    def run(*extra):
        return subprocess.run(
            [sys.executable, sys.argv[1], "--root", directory, *extra],
            text=True,
            capture_output=True,
        )

    assert run().returncode == 0  # Git metadata and ignored output are excluded.
    (root / "untracked.md").write_text(local)
    assert run().returncode == 0  # Untracked scratch files are not published.
    subprocess.run(["git", "-C", directory, "add", "untracked.md"], check=True)
    bad = run()
    assert bad.returncode == 1 and "untracked.md:1: local-path" in bad.stdout
    (root / "untracked.md").unlink()
    # The generic scanner works without local configuration.
    (root / "tracked.md").write_text("examplehost")
    assert run().returncode == 0
    terms_file = root / ".privacy-terms"
    terms_file.write_text("# synthetic local terms\n\nexamplehost\nexample-notes\n")
    assert len(module.load_terms(terms_file)) == 2
    assert run().returncode == 1
    (root / "tracked.md").write_text("example-notes")
    assert run().returncode == 1  # Working-tree contents, not the index snapshot.
    (root / "tracked.md").unlink()
    assert run().returncode == 0  # Staged-for-removal files need not exist.
    # Explicit files override the default; missing or invalid files fail closed.
    alternate = root / "generated" / "terms"
    alternate.write_text("other-example\n")
    (root / "tracked.md").write_text("other-example")
    assert run().returncode == 0
    assert run("--terms", str(alternate)).returncode == 1
    assert run("--terms", str(root / "missing")).returncode == 2
    alternate.write_text("[\n")
    assert run("--terms", str(alternate)).returncode == 2
    (root / "tracked.md").unlink()
    (root / "link").symlink_to(local)
    subprocess.run(["git", "-C", directory, "add", "link"], check=True)
    assert run().returncode == 1  # Check even a tracked dangling link's target.

# Source archives still receive a scan without requiring Git metadata.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / "source.md").write_text(local)
    result = subprocess.run(
        [sys.executable, sys.argv[1], "--root", directory],
        text=True,
        capture_output=True,
    )
    assert result.returncode == 1 and "source.md:1: local-path" in result.stdout
