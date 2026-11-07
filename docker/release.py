#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Build and smoke-test local release artifacts; never publishes."""

import argparse
import hashlib
import json
import os
import re
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
  --bundle also builds SeaHorn and a combined artifact, held for relicensing.
  --llvm-image reuses an installed LLVM image after checking LLVM_REVISION,
    Release mode, assertions, MLIR and lit. Without it LLVM builds from source.
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
  sea-mlir-release:*; no prune.
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
    def __init__(self, source, output, deadline):
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
            "distribution": "local only; SeaHorn bundle held pending relicensing",
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

    def build(self, name, dockerfile, target, arguments=()):
        image = "sea-mlir-release:" + name
        command = ["docker", "build", "--force-rm", "--build-arg", "JOBS=8"]
        for argument in arguments:
            command.extend(["--build-arg", argument])
        command.extend(["-f", dockerfile, "--target", target, "-t", image, "."])
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
    """Solver tags: the ARGs of docker/Dockerfile are their only definition."""
    text = (source / "docker" / "Dockerfile").read_text()
    result = []
    for name in ("Z3_TAG", "BITWUZLA_TAG"):
        found = re.search(rf"^ARG {name}=(\S+)$", text, re.MULTILINE)
        if not found:
            raise SystemExit(f"error: no ARG {name}= in docker/Dockerfile")
        result.append(f"{name}={found.group(1)}")
    return result


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
    parser.add_argument("--until", type=hhmm)
    parser.add_argument("--not-before", type=hhmm)
    parser.add_argument("--timezone")
    parser.add_argument("--plan", action="store_true")
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
    job = ReleaseJob(source, output, end)
    common = ["YICES=0"]
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
            job.build("seahorn", "docker/SeaHorn.Dockerfile", "seahorn")
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


if __name__ == "__main__":
    sys.exit(main())
