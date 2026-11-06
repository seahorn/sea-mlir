#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Publication checks use commit history, not the final worktree."""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

publish = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location("publish", publish)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

for url in (
    "https://github.com/seahorn/sea-mlir.git",
    "git@github.com:seahorn/sea-mlir.git",
    "ssh://git@github.com/seahorn/sea-mlir",
):
    assert module.public_url(url), url
for url in (
    "https://github.com/seahorn/sea-mlir-dev.git",
    "https://example.org/seahorn/sea-mlir",
    "https://github.com.evil.test/seahorn/sea-mlir",
):
    assert not module.public_url(url), url

with tempfile.TemporaryDirectory() as directory:
    top = Path(directory)
    repo = top / "repo"
    repo.mkdir()
    env = dict(
        os.environ,
        GIT_CONFIG_NOSYSTEM="1",
        GIT_CONFIG_GLOBAL=os.devnull,
        GIT_AUTHOR_NAME="Test",
        GIT_AUTHOR_EMAIL="test@example.org",
        GIT_COMMITTER_NAME="Test",
        GIT_COMMITTER_EMAIL="test@example.org",
    )
    env.pop("SEA_PRIVACY_TERMS", None)
    env.pop("SEA_PUBLISH_STRICT", None)

    def git(*args):
        return subprocess.run(
            ["git", "-C", str(repo), *args],
            env=env,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    git("init", "-q", "-b", "main")
    (repo / "data.txt").write_text("safe\n")
    git("add", ".")
    git("commit", "-qm", "Initial source")
    base = git("rev-parse", "HEAD")
    # Use an external policy and a synthetic private term, never a real identity.
    secret = "Synthetic" + "PrivateToken"
    terms = top / "terms"
    terms.write_text(secret + "\n")
    # A deterministic fake scanner tests integration without a network install.
    bin_dir = top / "bin"
    bin_dir.mkdir()
    scanner = bin_dir / "gitleaks"
    scanner.write_text(
        "#!"
        + sys.executable
        + "\n"
        + """import json, os, sys
from pathlib import Path
args = sys.argv
assert args[1] == "git"
assert "--ignore-gitleaks-allow" in args
assert "--redact" in args
assert "--diff-merges=first-parent" in next(a for a in args if a.startswith("--log-opts="))
assert Path(args[args.index("--config") + 1]).read_text() == "[extend]\\nuseDefault = true\\n"
assert Path(args[args.index("--gitleaks-ignore-path") + 1]).read_text() == ""
assert "GITLEAKS_CONFIG_TOML" not in os.environ
report = Path(args[args.index("--report-path") + 1])
code = int(os.environ.get("TEST_SCANNER_EXIT", "0"))
report.write_text(os.environ.get("TEST_SCANNER_REPORT", "[]"))
print(os.environ.get("TEST_PRIVATE_OUTPUT", ""), file=sys.stderr)
sys.exit(code)
"""
    )
    scanner.chmod(0o755)
    env["PATH"] = str(bin_dir) + os.pathsep + env["PATH"]
    env["GITLEAKS_CONFIG_TOML"] = "untrusted policy"
    env["TEST_PRIVATE_OUTPUT"] = secret

    def run(*args, code=0, stdin=None, extra_env=None):
        result = subprocess.run(
            [
                sys.executable,
                str(publish),
                "--root",
                str(repo),
                "--terms",
                str(terms),
                *args,
            ],
            env=env | (extra_env or {}),
            input=stdin,
            capture_output=True,
            text=True,
        )
        assert secret not in result.stdout + result.stderr, (
            result.stdout + result.stderr
        )
        assert result.returncode == code, result.stdout + result.stderr
        return result

    def save(message="Change source"):
        git("add", ".")
        git("commit", "-qm", message)
        return git("rev-parse", "HEAD")

    (repo / "data.txt").write_text("still safe\n")
    clean = save()
    run("--check-range", base, clean)
    # Uncommitted contents do not hide leaks or create false historical findings.
    (repo / "data.txt").write_text(secret)
    run("--check-range", base, clean)
    leak = save()
    (repo / "data.txt").write_text("clean again\n")
    tip = save()
    result = run("--check-range", base, tip, code=1)
    assert leak in result.stdout and "local-term" in result.stdout
    git("checkout", "-q", "-b", "message", clean)
    git("commit", "--allow-empty", "-qm", secret)
    run("--check-range", clean, "HEAD", code=1)
    # Merge-only edits are checked, even if neither parent contained them.
    git("checkout", "-q", "-b", "merge-side", clean)
    (repo / "side.txt").write_text("safe\n")
    save()
    git("checkout", "-q", "-b", "merge-main", clean)
    (repo / "main.txt").write_text("safe\n")
    save()
    git("merge", "--no-ff", "--no-commit", "merge-side")
    (repo / "merge.txt").write_text(secret)
    save("Merge source")
    run("--check-range", clean, "HEAD", code=1)
    git("checkout", "-q", "-b", "name", clean)
    (repo / secret).write_text("safe\n")
    save()
    run("--check-range", clean, "HEAD", code=1)
    git("checkout", "-q", "-b", "link", clean)
    (repo / "link").symlink_to(secret)
    save()
    run("--check-range", clean, "HEAD", code=1)
    # Public deletions are safe; tag publication is explicitly outside this tool.
    zero = "0" * 40
    run(
        "--pre-push",
        "origin",
        "https://github.com/seahorn/sea-mlir.git",
        stdin=f"(delete) {zero} refs/heads/old {clean}\n",
    )
    run(
        "--pre-push",
        "origin",
        "https://github.com/seahorn/sea-mlir.git",
        stdin=f"refs/tags/test {tip} refs/tags/test {zero}\n",
        code=2,
    )
    run(
        "--pre-push",
        "origin",
        "https://github.com/seahorn/sea-mlir.git",
        stdin=f"refs/heads/main {tip} refs/heads/main {base}\n",
        code=1,
    )
    # A private push must not need gitleaks or process even malformed ref updates.
    run(
        "--pre-push",
        "origin",
        "git@github.com:seahorn/sea-mlir-dev.git",
        stdin="unused\n",
    )
    # Real local remote: a new branch is checked relative to fetched public main.
    remote = top / "remote.git"
    git("init", "-q", "--bare", str(remote))
    git("remote", "add", "public", str(remote))
    git("push", "-q", "public", clean + ":refs/heads/main")
    git(
        "config",
        "url." + remote.as_uri() + ".insteadOf",
        "https://github.com/seahorn/sea-mlir.git",
    )
    run(
        "--pre-push",
        "public",
        "https://github.com/seahorn/sea-mlir.git",
        stdin=f"refs/heads/new {tip} refs/heads/new {zero}\n",
        code=1,
    )
    git("branch", "clean-publish", clean)
    run("clean-publish", "--dry-run")
    assert "refs/heads/clean-publish" not in git("ls-remote", "public")
    run("clean-publish")
    assert clean + "\trefs/heads/clean-publish" in git("ls-remote", "public")
    # Fetch and push URLs may differ: only actual public ancestry is trusted.
    dev_remote = top / "dev.git"
    git("clone", "-q", "--bare", str(remote), str(dev_remote))
    git("push", "-q", str(dev_remote), tip + ":refs/heads/main")
    git("remote", "set-url", "public", str(dev_remote))
    git("remote", "set-url", "--push", "public", str(remote))
    run("main", "--dry-run", code=1)
    git("remote", "set-url", "--add", "--push", "public", str(dev_remote))
    run("clean-publish", "--dry-run", code=2)
    git("config", "--unset-all", "remote.public.pushurl")
    git("remote", "set-url", "public", str(remote))
    git("branch", secret, clean)
    run(secret, "--dry-run", code=1)
    # Copy the actual hook and gate into the test tree; the hook only reads it.
    (repo / "utils").mkdir()
    shutil.copy(publish, repo / "utils" / "publish.py")
    shutil.copy(
        publish.with_name("privacy-check.py"), repo / "utils" / "privacy-check.py"
    )
    shutil.copytree(publish.parent / "hooks", repo / "utils" / "hooks")
    installed = subprocess.run(
        [sys.executable, str(repo / "utils" / "publish.py"), "--install-hook"],
        cwd=repo,
        env=env,
        capture_output=True,
        text=True,
    )
    assert installed.returncode == 0, installed.stderr
    hook = repo / ".git" / "hooks" / "pre-push"
    result = subprocess.run(
        [str(hook), "origin", "git@github.com:seahorn/sea-mlir-dev.git"],
        cwd=repo,
        env=env,
        input="unused\n",
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    (repo / "utils" / "publish.py").rename(repo / "utils" / "disabled.py")
    result = subprocess.run(
        [str(hook), "origin", "git@github.com:seahorn/sea-mlir-dev.git"],
        cwd=repo,
        env=env,
        input="unused\n",
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    result = subprocess.run(
        [str(hook), "public", "git@github.com:seahorn/sea-mlir.git"],
        cwd=repo,
        env=env,
        input="unused\n",
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2, result.stderr
    (repo / "utils" / "disabled.py").rename(repo / "utils" / "publish.py")
    hook.write_text("#!/bin/sh\nexit 0\n")
    result = subprocess.run(
        [sys.executable, str(repo / "utils" / "publish.py"), "--install-hook"],
        cwd=repo,
        env=env,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2 and hook.read_text().endswith("exit 0\n")
    # Scanner findings and failures are not swallowed, and raw errors stay private.
    findings = [
        {
            "Commit": clean,
            "File": secret + ".txt",
            "StartLine": 1,
            "RuleID": "test-secret",
        }
    ]
    run(
        "--check-range",
        base,
        clean,
        code=1,
        extra_env={
            "TEST_SCANNER_EXIT": "1",
            "TEST_SCANNER_REPORT": json.dumps(findings),
        },
    )
    run("--check-range", base, clean, code=2, extra_env={"TEST_SCANNER_EXIT": "9"})
    run("--check-range", base, clean, code=2, extra_env={"TEST_SCANNER_EXIT": "1"})
    # Invalid policy errors must not quote the private regular expression.
    terms.write_text("(?P<" + secret + ">")
    run("--check-range", base, clean, code=2)
    run(
        "--pre-push",
        "origin",
        "git@github.com:seahorn/sea-mlir-dev.git",
        stdin="unused\n",
    )
    terms.unlink()
    run(
        "--pre-push",
        "origin",
        "git@github.com:seahorn/sea-mlir-dev.git",
        stdin="unused\n",
    )
    terms.write_text(secret + "\n")
    # An initial CI push checks the root commit as well as later history.
    run("--check-range", zero, base)
    run("--check-range", zero, tip, code=1)
    git("checkout", "-q", "--orphan", "root-leak")
    (repo / "root-leak.txt").write_text(secret)
    git("add", "root-leak.txt")
    git("commit", "-qm", "Initial synthetic leak")
    run("--check-range", zero, "HEAD", code=1)
    with patch.object(module.shutil, "which", return_value=None):
        assert module.Checker(repo, terms, False).scan_secrets(base + ".." + clean) == 0
        try:
            module.Checker(repo, terms, True).scan_secrets(base + ".." + clean)
            raise AssertionError("strict scan allowed missing gitleaks")
        except module.GateError:
            pass
    # Git silently truncates rev-list in a shallow clone; refuse such a scan.
    shallow = top / "shallow"
    subprocess.run(
        [
            "git",
            "clone",
            "-q",
            "--depth=1",
            "--branch",
            "main",
            remote.as_uri(),
            str(shallow),
        ],
        env=env,
        check=True,
    )
    result = subprocess.run(
        [
            sys.executable,
            str(publish),
            "--root",
            str(shallow),
            "--check-range",
            zero,
            "HEAD",
        ],
        env=env,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2 and "shallow" in result.stderr
print("publish-tests: pass")
