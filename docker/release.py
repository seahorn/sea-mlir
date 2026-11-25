#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Build and smoke-test local release artifacts; never publishes."""

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timedelta
from datetime import time as clock
from pathlib import Path
from zoneinfo import ZoneInfo

AGENT = """release: build local sea-mlir tarballs and images; never publish.
USAGE: python3 docker/release.py OUTPUT [--bundle] [--llvm-image IMAGE]
       python3 docker/release.py OUTPUT --homebrew [--brew-source REF]
  --bundle also builds SeaHorn (with Yices, without clam) and the bundle.
  --homebrew builds the Homebrew bottles instead of the tarballs: the tap's
    formulae (packaging/homebrew/Formula) in ghcr.io/homebrew/brew, built from
    source, tested and bottled in dependency order (docker/Homebrew.Dockerfile),
    the license gate over every bottle's files, and a smoke in a fresh brew
    container that installs from the bottles only. Output: OUTPUT/artifacts/
    homebrew (bottles, JSON, formulae with bottle blocks, INSTALL-TEST.md).
  --brew-source REF: the sea-mlir commit of the sea-mlir bottle, as a local
    `git archive` tarball (default HEAD); version <project version>-local.<sha>
    unless REF is the tag v<project version>. `formula` keeps the formula's
    own url (the release tag's tarball).
  --llvm-image reuses an installed LLVM image after checking LLVM_REVISION,
    Release mode, assertions, MLIR and lit. Without it LLVM builds from source.
  --image-prefix NAME tags the images NAME:<stage> (default sea-mlir-release).
  --cpuset CPUS pins every docker build to those CPUs (docker --cpuset-cpus,
    e.g. 40-47), a hard cap next to the eight compile jobs.
  --version defaults to the project version in CMakeLists.txt (artifact name,
    not a claim of release approval).
  --until HH:MM stops the run at the next such local time (default: no limit).
  --not-before HH:MM, with --until, refuses to start before that local time;
    when it is later than --until the window spans midnight.
  --timezone NAME sets the zone of those times (default: the system zone).
  --plan prints the stages without running them; no time check.
LIMITS: eight compile jobs, four lit workers, sequential build stages.
  With --until the active command is interrupted at that time. A failed or
  interrupted run can be rerun; Docker layers are cached. Images use
  <image-prefix>:* (default sea-mlir-release:*); no prune.
OUTPUT: OUTPUT/artifacts, per-run logs, and release-run.json with timings,
  source revision, image IDs, hashes and completion status. OUTPUT must be
  outside the source tree. Only temporary extraction/smoke containers are removed.
EXIT: 0 complete/plan; 1 failed stage; 2 configuration error/outside window;
  124 deadline (--until) reached. No push, tag, release creation or attestation.
"""


def hhmm(text):
    try:
        return clock.fromisoformat(text)
    except ValueError:
        raise argparse.ArgumentTypeError("expected HH:MM") from None


def cutoff(now, until, not_before=None):
    """Next local `until` after `now`; with `not_before`, `now` must be in the window."""
    wall = now.time().replace(tzinfo=None)
    if not_before is not None:
        if not_before <= until:
            inside = not_before <= wall < until
        else:
            inside = wall >= not_before or wall < until
        if not inside:
            raise ValueError(
                f"outside the {not_before:%H:%M}-{until:%H:%M} build window"
            )
    naive = now.replace(tzinfo=None, fold=0)
    end = naive.replace(hour=until.hour, minute=until.minute, second=0, microsecond=0)
    if end <= naive:
        end += timedelta(days=1)
    if isinstance(now.tzinfo, ZoneInfo):
        return end.replace(tzinfo=now.tzinfo)
    return end.astimezone()  # naive: interpreted in the system zone


class ReleaseJob:
    def __init__(
        self, source, output, deadline, prefix="sea-mlir-release", cpuset=None
    ):
        self.prefix = prefix
        self.cpuset = cpuset
        self.source = source
        self.output = output
        self.deadline = deadline
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        self.logs = output / "logs" / stamp
        self.logs.mkdir(parents=True, exist_ok=True)
        self.record = {
            "source": subprocess.check_output(
                ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
            ).strip(),
            "source_epoch": subprocess.check_output(
                ["git", "-C", str(source), "show", "-s", "--format=%ct", "HEAD"],
                text=True,
            ).strip(),
            "source_dirty": bool(
                subprocess.check_output(
                    [
                        "git",
                        "--no-optional-locks",
                        "-C",
                        str(source),
                        "status",
                        "--porcelain",
                    ],
                    text=True,
                ).strip()
            ),
            "cutoff": deadline.isoformat() if deadline else None,
            "status": "running",
            "stages": [],
            "images": {},
            "distribution": "local only; publishing is a separate, manual step",
        }
        self.save()

    def save(self):
        (self.output / "release-run.json").write_text(
            json.dumps(self.record, indent=2) + "\n"
        )

    def run(self, name, command):
        remaining = None
        if self.deadline is not None:
            remaining = self.deadline.timestamp() - time.time()
            if remaining <= 0:
                raise TimeoutError("deadline reached")
        path = self.logs / (name + ".log")
        print(f"stage: {name} log={path}", flush=True)
        start = time.monotonic()
        timed_out = False
        with path.open("w") as log:
            process = subprocess.Popen(
                command,
                cwd=self.source,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                code = process.wait(timeout=remaining)
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                timed_out = True
                try:
                    os.killpg(process.pid, signal.SIGINT)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                code = 124
        self.record["stages"].append(
            {
                "name": name,
                "command": command,
                "seconds": round(time.monotonic() - start, 2),
                "exit": code,
                "log": str(path),
            }
        )
        self.save()
        if timed_out:
            # Legacy Docker logs identify the intermediate containers of this
            # command. Stop only those, if daemon-side work survived cancellation.
            log_text = path.read_text(errors="replace")
            ids = set(re.findall(r"Running in ([0-9a-f]{12,64})", log_text))
            ids.update(
                re.findall(
                    r"^smoke-container: ([A-Za-z0-9_.-]+)$", log_text, re.MULTILINE
                )
            )
            for container in ids:
                try:
                    subprocess.run(
                        ["docker", "stop", "--time", "5", container],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        timeout=15,
                    )
                except (OSError, subprocess.TimeoutExpired) as error:
                    print(
                        f"cleanup: could not stop {container}: {error}", file=sys.stderr
                    )
            raise TimeoutError("deadline reached or build interrupted")
        if code:
            raise RuntimeError(f"{name} failed (exit {code}); see {path}")

    def build(self, name, dockerfile, target, arguments=(), context="."):
        image = self.prefix + ":" + name
        command = ["docker", "build", "--force-rm", "--build-arg", "JOBS=8"]
        if self.cpuset:
            command += ["--cpuset-cpus", self.cpuset]
        for argument in arguments:
            command.extend(["--build-arg", argument])
        command.extend(["-f", dockerfile, "--target", target, "-t", image, context])
        self.run(name, command)
        self.record["images"][image] = subprocess.check_output(
            ["docker", "image", "inspect", "--format", "{{.Id}}", image], text=True
        ).strip()
        self.save()
        return image

    def extract(self, image):
        container = subprocess.check_output(
            ["docker", "create", image], text=True
        ).strip()
        try:
            artifacts = self.output / "artifacts"
            artifacts.mkdir(exist_ok=True)
            self.run(
                "extract-" + image.split(":")[-1],
                ["docker", "cp", container + ":/artifacts/.", str(artifacts)],
            )
        finally:
            subprocess.run(
                ["docker", "rm", container], check=True, stdout=subprocess.DEVNULL
            )


def project_version(source):
    text = (source / "CMakeLists.txt").read_text()
    found = re.search(r"project\(\s*sea-mlir\s+VERSION\s+([0-9]+(?:\.[0-9]+)*)", text)
    if not found:
        raise SystemExit("error: no project(sea-mlir VERSION ...) in CMakeLists.txt")
    return found.group(1)


def pins(source):
    """Solver pins: the ARGs of docker/Dockerfile are their only definition."""
    text = (source / "docker" / "Dockerfile").read_text()
    result = []
    for name in ("Z3_REVISION", "BITWUZLA_TAG", "YICES_TAG"):
        found = re.search(rf"^ARG {name}=(\S+)$", text, re.MULTILINE)
        if not found:
            raise SystemExit(f"error: no ARG {name}= in docker/Dockerfile")
        result.append(f"{name}={found.group(1)}")
    return result


HOMEBREW_FORMULAE = ("sea-mlir-llvm", "sea-mlir", "seahorn")  # dependency order


def brew_source(source, ref):
    """(version, commit, is_tag) of `ref` for the local sea-mlir tarball."""
    commit = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "--verify", ref + "^{commit}"],
        text=True,
    ).strip()
    text = subprocess.check_output(
        ["git", "-C", str(source), "show", f"{commit}:CMakeLists.txt"], text=True
    )
    found = re.search(r"project\(\s*sea-mlir\s+VERSION\s+([0-9.]+)", text)
    if not found:
        raise SystemExit(f"error: no project version at {ref}")
    version = found.group(1)
    tag = subprocess.run(
        [
            "git",
            "-C",
            str(source),
            "rev-parse",
            "--verify",
            "-q",
            f"v{version}^{{commit}}",
        ],
        capture_output=True,
        text=True,
    ).stdout.strip()
    if tag == commit:
        return version, commit, True
    return f"{version}-local.{commit[:12]}", commit, False


def set_formula_source(text, url, sha256, version):
    """The formula with its top-level url / sha256 replaced and a version."""
    lines = text.splitlines(keepends=True)
    out = []
    done = set()
    for line in lines:
        key = re.match(r"  (url|sha256) ", line)
        if key and key.group(1) not in done:
            done.add(key.group(1))
            if key.group(1) == "url":
                out.append(f'  url "{url}"\n')
                out.append(f'  version "{version}"\n')
            else:
                out.append(f'  sha256 "{sha256}"\n')
            continue
        out.append(line)
    if done != {"url", "sha256"}:
        raise RuntimeError("formula has no top-level url and sha256")
    return "".join(out)


def homebrew_context(source, output, ref):
    """OUTPUT/homebrew-context: the tap's formulae, the build script and,
    unless ref is `formula`, a `git archive` of ref that sea-mlir.rb builds.
    Returns a record of the source."""
    context = output / "homebrew-context"
    if context.exists():
        shutil.rmtree(context)
    (context / "tap" / "Formula").mkdir(parents=True)
    (context / "src").mkdir()
    (context / "docker").mkdir()
    shutil.copy2(source / "docker" / "homebrew-bottle.sh", context / "docker")
    for name in HOMEBREW_FORMULAE:
        shutil.copy2(
            source / "packaging" / "homebrew" / "Formula" / f"{name}.rb",
            context / "tap" / "Formula",
        )
    if ref == "formula":
        return {"ref": "formula", "url": "as in sea-mlir.rb"}
    version, commit, tagged = brew_source(source, ref)
    archive = context / "src" / f"sea-mlir-{version}.tar.gz"
    subprocess.run(
        [
            "git",
            "-C",
            str(source),
            "archive",
            "--format=tar.gz",
            f"--prefix=sea-mlir-{version}/",
            "-o",
            str(archive),
            commit,
        ],
        check=True,
    )
    digest = file_hash(archive)
    url = f"file:///home/linuxbrew/src/{archive.name}"
    formula = context / "tap" / "Formula" / "sea-mlir.rb"
    formula.write_text(set_formula_source(formula.read_text(), url, digest, version))
    return {
        "ref": ref,
        "commit": commit,
        "tag": tagged,
        "version": version,
        "url": url,
        "sha256": digest,
    }


def file_hash(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--agent", action="store_true")
    parser.add_argument("--bundle", action="store_true")
    parser.add_argument("--llvm-image")
    parser.add_argument("--version")
    parser.add_argument("--image-prefix", default="sea-mlir-release")
    parser.add_argument("--cpuset")
    parser.add_argument("--until", type=hhmm)
    parser.add_argument("--not-before", type=hhmm)
    parser.add_argument("--timezone")
    parser.add_argument("--plan", action="store_true")
    parser.add_argument("--homebrew", action="store_true")
    parser.add_argument("--brew-source", default="HEAD")
    args = parser.parse_args()
    if args.agent:
        print(AGENT)
        return 0
    if not args.output:
        parser.error("OUTPUT is required")
    source = Path(__file__).resolve().parent.parent
    if args.version is None:
        args.version = project_version(source)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.version):
        parser.error("version must be a plain filename component")
    if args.not_before and not args.until:
        parser.error("--not-before requires --until")
    try:
        zone = ZoneInfo(args.timezone) if args.timezone else None
    except (KeyError, ValueError, OSError) as error:
        parser.error(f"unknown timezone {args.timezone}: {error}")
    output = args.output.resolve()
    if output.is_relative_to(source):
        parser.error("OUTPUT must be outside the source tree")
    if not re.fullmatch(r"[a-z0-9][a-z0-9._/-]*", args.image_prefix):
        parser.error("--image-prefix must be a docker repository name")
    if args.cpuset and not re.fullmatch(r"[0-9]+([-,][0-9]+)*", args.cpuset):
        parser.error("--cpuset: a docker cpuset such as 40-47")
    if args.homebrew and (args.bundle or args.llvm_image):
        parser.error("--homebrew builds the bottles only (no --bundle, --llvm-image)")
    if args.plan and args.homebrew:
        print("stages: homebrew-bottle homebrew-license-gate homebrew-smoke")
        print("formulae: " + " ".join(HOMEBREW_FORMULAE))
        return 0
    if args.plan:
        print(
            "stages: llvm solvers sea-mlir package runtime standalone-smoke"
            + (
                " seahorn bundle-package bundle-runtime bundle-smoke"
                if args.bundle
                else ""
            )
        )
        print(
            "limits: compile=8 tests=4 until="
            + (f"{args.until:%H:%M}" if args.until else "none")
        )
        print("version: " + args.version)
        return 0
    end = None
    if args.until:
        try:
            end = cutoff(
                datetime.now(zone).astimezone(zone), args.until, args.not_before
            )
        except ValueError as error:
            parser.error(str(error))
    job = ReleaseJob(source, output, end, args.image_prefix, args.cpuset)
    if args.homebrew:
        return homebrew(job, source, output, args)
    common = ["YICES=1"]
    if args.llvm_image:
        common += ["LLVM_BASE=" + args.llvm_image, "REUSE_LLVM=1"]
    describe = job.record["source"][:12] + "-release-build"
    if job.record["source_dirty"]:
        describe += "-dirty"
    common += ["SEA_GIT_DESCRIBE=" + describe]
    packaging = [
        "VERSION=" + args.version,
        "SOURCE_REVISION=" + job.record["source"],
        "SOURCE_DATE_EPOCH=" + job.record["source_epoch"],
        *pins(source),
        f"SEA_MLIR_IMAGE={args.image_prefix}:sea-mlir",
        f"SEAHORN_IMAGE={args.image_prefix}:seahorn",
    ]
    try:
        for stage in ("llvm", "solvers", "sea-mlir"):
            job.build(stage, "docker/Dockerfile", stage, common)
        image = job.build("package", "docker/Release.Dockerfile", "package", packaging)
        job.extract(image)
        job.build("runtime", "docker/Release.Dockerfile", "runtime", packaging)
        # Smoke commands are supplied by the packaged fixture driver. It unpacks
        # the exported tarball in a clean base, rather than testing the build tree.
        job.run(
            "standalone-smoke",
            ["bash", "docker/smoke/run.sh", str(output / "artifacts"), args.version],
        )
        if args.bundle:
            # SeaHorn links the solvers stage's Yices (YICES_TAG)
            job.build(
                "seahorn",
                "docker/SeaHorn.Dockerfile",
                "seahorn",
                [
                    f"SOLVERS_IMAGE={args.image_prefix}:solvers",
                    *(pin for pin in pins(source) if pin.startswith("YICES_TAG=")),
                ],
            )
            image = job.build(
                "bundle-package",
                "docker/Release.Dockerfile",
                "bundle-package",
                packaging,
            )
            job.extract(image)
            job.build(
                "bundle-runtime",
                "docker/Release.Dockerfile",
                "bundle-runtime",
                packaging,
            )
            job.run(
                "bundle-smoke",
                [
                    "bash",
                    "docker/smoke/run.sh",
                    str(output / "artifacts"),
                    args.version,
                    "--bundle",
                ],
            )
        job.record["artifacts"] = {
            str(p.relative_to(output / "artifacts")): {
                "bytes": p.stat().st_size,
                "sha256": file_hash(p),
            }
            for p in sorted((output / "artifacts").rglob("*"))
            if p.is_file()
        }
        job.record["status"] = "complete"
        print("release: complete; artifacts remain local")
        return 0
    except TimeoutError as error:
        job.record["status"] = "deadline-expired"
        print(f"error: {error}", file=sys.stderr)
        return 124
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        job.record["status"] = "failed"
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        job.save()


def homebrew(job, source, output, args):
    """The --homebrew stages; the record's status as in main()."""
    artifacts = output / "artifacts" / "homebrew"
    try:
        job.record["homebrew_source"] = homebrew_context(
            source, output, args.brew_source
        )
        job.save()
        image = job.build(
            "homebrew-bottle",
            "docker/Homebrew.Dockerfile",
            "homebrew-bottle",
            context=str(output / "homebrew-context"),
        )
        if artifacts.exists():
            shutil.rmtree(artifacts)
        job.extract(image)
        job.run(
            "homebrew-license-gate",
            [
                sys.executable,
                str(source / "docker" / "package-release.py"),
                "--audit-bottles",
                str(artifacts / "bottles"),
                "--output",
                str(artifacts / "license-audit.json"),
            ],
        )
        smoke = ["bash", "docker/homebrew-smoke.sh", str(artifacts)]
        if args.cpuset:
            smoke.append(args.cpuset)
        job.run("homebrew-smoke", smoke)
        (artifacts / "INSTALL-TEST.md").write_text(
            (source / "packaging" / "homebrew" / "INSTALL-TEST.md").read_text()
        )
        (artifacts / "install-local.sh").write_text(
            (source / "packaging" / "homebrew" / "install-local.sh").read_text()
        )
        job.record["artifacts"] = {
            str(p.relative_to(output / "artifacts")): {
                "bytes": p.stat().st_size,
                "sha256": file_hash(p),
            }
            for p in sorted(artifacts.rglob("*"))
            if p.is_file() and "logs" not in p.relative_to(artifacts).parts
        }
        job.record["status"] = "complete"
        print("release: homebrew bottles complete; artifacts remain local")
        return 0
    except TimeoutError as error:
        job.record["status"] = "deadline-expired"
        print(f"error: {error}", file=sys.stderr)
        return 124
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        job.record["status"] = "failed"
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        job.save()


if __name__ == "__main__":
    sys.exit(main())
