#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Check every unpublished commit before pushing a curated branch."""

import argparse
import importlib.machinery
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

AGENT = """publish: check commit history before publishing a curated branch.
USAGE: python3 utils/publish.py BRANCH [--remote public] [--terms FILE] [--strict] [--dry-run]
  Fetches REMOTE/main, checks every commit in REMOTE/main..BRANCH, then pushes
  the checked SHA to refs/heads/BRANCH (never force), and prints a PR command.
  --dry-run checks only: it may fetch but never pushes.
  --check-range BASE HEAD checks only, using locally available commits.
  An all-zero BASE scans the entire ancestry (initial CI push).
  --root DIR selects the Git repository (default: current working directory).
  --pre-push REMOTE URL reads Git's ref updates from stdin; public GitHub
  sea-mlir URLs are checked, other remotes are ignored. Branch refs only.
  --install-hook installs utils/hooks/pre-push without replacing another hook.
CHECKS: full content of every added/changed file in each commit, filenames,
  and commit messages; symlink targets are blobs, never followed. Merge
  changes are compared against every parent. The matcher is privacy-check.py.
  --terms or SEA_PRIVACY_TERMS supplies optional private regexes; no default
  terms file is loaded. gitleaks scans the same range with its built-in rules,
  ignoring repository configuration and suppressing raw scanner output.
  Missing gitleaks warns; --strict (or SEA_PUBLISH_STRICT=1) makes it an error.
OUTPUT: key: value lines; findings name commit, file, line, category only.
  Private terms and matching contents are not printed. No report is retained.
EXIT: 0 clean / push succeeded / non-public hook; 1 finding; 2 tool/config error.
LIMITS: an accidental-disclosure guard, not a security boundary against a
  malicious developer. Git hooks are local and bypassable. CI checks happen
  after publication; scan locally before the first public push.
"""


class GateError(Exception):
    """A safe diagnostic, with no captured Git or scanner output."""


def git(root, *args):
    result = subprocess.run(
        ["git", "--no-replace-objects", "-C", str(root), *args],
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise GateError(
            "git operation failed; inspect repository configuration locally"
        )
    return result.stdout


def commit(root, ref):
    return (
        git(root, "rev-parse", "--verify", "--end-of-options", ref + "^{commit}")
        .decode()
        .strip()
    )


def matcher():
    spec = importlib.util.spec_from_file_location(
        "privacy_check", Path(__file__).with_name("privacy-check.py")
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def public_url(url):
    # The installed hook must recognize private remotes without importing files
    # from a worktree that may predate this tool. Keep its predicate authoritative.
    loader = importlib.machinery.SourceFileLoader(
        "publish_hook", str(Path(__file__).parent / "hooks" / "pre-push")
    )
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module.public_url(url)


class Checker:
    def __init__(self, root, terms, strict):
        self.root = root
        self.strict = strict
        self.scanner = matcher()
        try:
            self.terms = self.scanner.load_terms(terms) if terms else []
        except (OSError, UnicodeError, re.error):
            raise GateError(
                "cannot read private terms or compile a private regex"
            ) from None

    def safe(self, text):
        # A filename or scanner rule identifier can itself contain a private term.
        for pattern in [*self.scanner.RULES.values(), *self.terms]:
            text = pattern.sub("[redacted]", text)
        return json.dumps(text, ensure_ascii=True)

    def finding(self, sha, path, line, category):
        print(
            f"finding: commit={sha} file={self.safe(path)} line={line} pattern={self.safe(category)}"
        )

    def scan_text(self, sha, path, text):
        count = 0
        for line, category, _ in self.scanner.findings(path, text, self.terms):
            self.finding(sha, path, line, category)
            count += 1
        return count

    def scan_commit(self, sha):
        count = self.scan_text(
            sha,
            "COMMIT_MESSAGE.md",
            git(self.root, "show", "-s", "--format=%B", sha).decode("utf-8", "replace"),
        )
        names = git(
            self.root,
            "diff-tree",
            "--root",
            "--no-commit-id",
            "--name-only",
            "--no-renames",
            "-r",
            "-m",
            "--diff-filter=ACMT",
            "-z",
            sha,
        )
        for name in sorted(set(names.split(b"\0")) - {b""}):
            path = os.fsdecode(name)
            count += self.scan_text(sha, "FILE_NAME.md", path)
            spec = f"{sha}:{path}"
            kind = git(self.root, "cat-file", "-t", spec).strip()
            if kind == b"commit":  # Submodule contents are not in this repository.
                continue
            if kind != b"blob":
                raise GateError("unsupported changed Git object")
            text = git(self.root, "cat-file", "blob", spec).decode("utf-8", "replace")
            count += self.scan_text(sha, path, text)
        return count

    def scan_secrets(self, revision_range):
        binary = shutil.which("gitleaks")
        if not binary:
            if self.strict:
                raise GateError("gitleaks is required in strict mode")
            print("warning: gitleaks unavailable; secret scan skipped")
            return 0
        with tempfile.TemporaryDirectory(prefix="sea-publish-") as directory:
            temp = Path(directory)
            config = temp / "gitleaks.toml"
            config.write_text("[extend]\nuseDefault = true\n")
            ignore = temp / "ignore"
            ignore.write_text("")
            report = temp / "findings.json"
            env = dict(os.environ)
            env.pop("GITLEAKS_CONFIG", None)
            env.pop("GITLEAKS_CONFIG_TOML", None)
            env["GIT_NO_REPLACE_OBJECTS"] = "1"
            result = subprocess.run(
                [
                    binary,
                    "git",
                    str(self.root),
                    "--log-opts="
                    + "--full-history --no-ext-diff --no-textconv --diff-merges=first-parent "
                    + revision_range,
                    "--config",
                    str(config),
                    "--gitleaks-ignore-path",
                    str(ignore),
                    "--ignore-gitleaks-allow",
                    "--redact",
                    "--no-banner",
                    "--exit-code",
                    "1",
                    "--report-format",
                    "json",
                    "--report-path",
                    str(report),
                ],
                capture_output=True,
                env=env,
            )
            if result.returncode not in (0, 1):
                raise GateError("gitleaks failed; raw diagnostics suppressed")
            try:
                findings = json.loads(report.read_text())
                if not isinstance(findings, list):
                    raise ValueError
                for item in findings:
                    sha = item["Commit"]
                    if not re.fullmatch(r"[0-9a-f]{40,64}", sha):
                        raise ValueError
                    self.finding(
                        sha,
                        str(item["File"]),
                        int(item["StartLine"]),
                        "secret/" + str(item["RuleID"]),
                    )
            except (OSError, ValueError, KeyError, TypeError):
                raise GateError(
                    "invalid gitleaks report; raw diagnostics suppressed"
                ) from None
            if result.returncode and not findings:
                raise GateError("gitleaks refused the range without a usable report")
            return len(findings)

    def check(self, base, head):
        if git(self.root, "rev-parse", "--is-shallow-repository").strip() == b"true":
            raise GateError(
                "shallow history cannot establish publication safety; fetch --unshallow first"
            )
        # Resolve before constructing ranges: callers cannot supply log options.
        head = commit(self.root, head)
        empty_base = bool(re.fullmatch(r"0{40}|0{64}", base))
        revision_range = head if empty_base else commit(self.root, base) + ".." + head
        commits = (
            git(self.root, "rev-list", "--reverse", revision_range)
            .decode()
            .splitlines()
        )
        print(f"range: {revision_range}")
        print(f"commits: {len(commits)}")
        count = sum(self.scan_commit(sha) for sha in commits)
        count += self.scan_secrets(revision_range)
        print(f"findings: {count}")
        print("publish-check: " + ("refused" if count else "pass"))
        return int(count != 0)


def remote_main(root, url):
    # Fetch the actual push destination, not a remote's possibly different
    # fetch URL. Pin its advertised SHA rather than racing on FETCH_HEAD.
    advertised = git(root, "ls-remote", "--refs", "--", url, "refs/heads/main").split()
    if len(advertised) != 2 or advertised[1] != b"refs/heads/main":
        raise GateError("publication destination has no unique main branch")
    sha = advertised[0].decode("ascii")
    if not re.fullmatch(r"[0-9a-f]{40,64}", sha):
        raise GateError("invalid advertised main object ID")
    git(root, "fetch", "--no-tags", "--", url, sha)
    return commit(root, sha)


def push_destination(root, remote):
    urls = (
        git(root, "remote", "get-url", "--push", "--all", "--", remote)
        .decode()
        .splitlines()
    )
    if len(urls) != 1:
        raise GateError("publication requires exactly one push URL")
    return urls[0]


def hook(checker, remote, url):
    if not public_url(url):
        print("publish-check: non-public remote; skipped")
        return 0
    result = 0
    for line in sys.stdin:
        fields = line.split()
        if len(fields) != 4:
            raise GateError("malformed pre-push ref update")
        _, local_sha, remote_ref, remote_sha = fields
        if not all(
            re.fullmatch(r"[0-9a-f]{40,64}", s) for s in (local_sha, remote_sha)
        ):
            raise GateError("malformed pre-push object ID")
        if set(local_sha) == {"0"}:  # Deletion publishes no new content.
            continue
        if not remote_ref.startswith("refs/heads/"):
            raise GateError(
                "public pushes support branch refs only; tags need a separate release review"
            )
        if set(remote_sha) == {"0"}:
            base = remote_main(checker.root, url)
        else:
            # The remote old tip may be absent in a shallow/local checkout.
            try:
                base = commit(checker.root, remote_sha)
            except GateError:
                git(checker.root, "fetch", "--no-tags", "--", url, remote_sha)
                base = commit(checker.root, remote_sha)
        result |= int(checker.scan_text(local_sha, "BRANCH_NAME.md", remote_ref) != 0)
        result |= checker.check(base, local_sha)
    return result


def install_hook(root):
    destination = Path(
        os.fsdecode(git(root, "rev-parse", "--git-path", "hooks/pre-push")).strip()
    )
    if not destination.is_absolute():
        destination = root / destination
    source = Path(__file__).parent / "hooks" / "pre-push"
    if destination.is_symlink() or (
        destination.exists() and destination.read_bytes() != source.read_bytes()
    ):
        raise GateError("another pre-push hook exists; integrate it manually")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(source.read_bytes())
    destination.chmod(0o755)
    print("hook: installed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("branch", nargs="?")
    parser.add_argument("--agent", action="store_true")
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--remote", default="public")
    parser.add_argument(
        "--terms", type=Path, default=os.environ.get("SEA_PRIVACY_TERMS")
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=os.environ.get("SEA_PUBLISH_STRICT") == "1",
    )
    parser.add_argument("--dry-run", action="store_true")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check-range", nargs=2, metavar=("BASE", "HEAD"))
    mode.add_argument("--pre-push", nargs=2, metavar=("REMOTE", "URL"))
    mode.add_argument("--install-hook", action="store_true")
    args = parser.parse_args()
    if args.agent:
        print(AGENT)
        return 0
    if args.branch and (args.check_range or args.pre_push or args.install_hook):
        parser.error("a branch cannot be combined with a check or hook mode")
    try:
        root = Path(os.fsdecode(git(args.root, "rev-parse", "--show-toplevel")).strip())
        if args.install_hook:
            return install_hook(root)
        if args.pre_push and not public_url(args.pre_push[1]):
            print("publish-check: non-public remote; skipped")
            return 0
        checker = Checker(root, args.terms, args.strict)
        if args.pre_push:
            return hook(checker, *args.pre_push)
        if args.check_range:
            return checker.check(*args.check_range)
        if not args.branch:
            parser.error("a branch or check mode is required")
        git(root, "check-ref-format", "refs/heads/" + args.branch)
        head = commit(root, "refs/heads/" + args.branch)
        if checker.scan_text(head, "BRANCH_NAME.md", args.branch):
            print("publish-check: refused")
            return 1
        destination = push_destination(root, args.remote)
        base = remote_main(root, destination)
        if checker.check(base, head):
            return 1
        if args.dry_run:
            print("publish: dry-run; no push")
            return 0
        # Push the checked SHA, even if another process moves the branch meanwhile.
        env = dict(os.environ)
        if args.terms:
            env["SEA_PRIVACY_TERMS"] = str(args.terms.resolve())
        if args.strict:
            env["SEA_PUBLISH_STRICT"] = "1"
        result = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "push",
                "--no-follow-tags",
                "--",
                destination,
                head + ":refs/heads/" + args.branch,
            ],
            capture_output=True,
            env=env,
        )
        if result.returncode:
            raise GateError(
                "push failed; Git output suppressed to protect private data"
            )
        print("publish: pushed " + head)
        print(
            "next: "
            + shlex.join(
                [
                    "gh",
                    "pr",
                    "create",
                    "--repo",
                    "seahorn/sea-mlir",
                    "--base",
                    "main",
                    "--head",
                    args.branch,
                ]
            )
        )
        return 0
    except (GateError, OSError) as error:
        # Never interpolate OSError: it can include a private filename.
        print(
            "error: "
            + (
                str(error)
                if isinstance(error, GateError)
                else "filesystem or subprocess operation failed"
            ),
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    sys.exit(main())
