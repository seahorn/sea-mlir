# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""A passing or failing nonrecording gate must leave no persistent artifacts."""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

bench, vc, fixture = map(lambda p: str(Path(p).resolve()), sys.argv[1:])


def snapshot(root):
    return {
        str(p.relative_to(root)): p.read_bytes() if p.is_file() else None
        for p in root.rglob("*")
    }


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    suite = root / "suite"
    family = suite / "family"
    family.mkdir(parents=True)
    shutil.copyfile(fixture, family / "one.mlir")
    # Force status to refresh a stale tracked-file stat cache. Even that read
    # must not rewrite a suite's Git index during a nonrecording gate.
    subprocess.run(["git", "init", "-q", str(suite)], check=True)
    subprocess.run(["git", "-C", str(suite), "add", "."], check=True)
    os.utime(family / "one.mlir", (1, 1))
    home = root / "bench"
    home.mkdir()
    env = dict(
        os.environ,
        SEA_BENCH_SUITES=str(suite),
        SEA_BENCH_HOME=str(home),
        TMPDIR=str(root),
    )
    command = [
        sys.executable,
        bench,
        "gate",
        "smoke",
        "--sea-vc",
        vc,
        "--min-free-gb",
        "0",
        "--par",
        "1",
    ]
    for expected, code in [("unsat", 0), ("sat", 1)]:
        (family / "manifest.yaml").write_text(
            "family: family\ndefaults:\n  args: [--lower, --seam]\n"
            "  timeout: 30\n  mem: 4096\njobs:\n"
            f"  one: {{expected: {expected}, tier: smoke}}\n"
        )
        before = snapshot(root)
        result = subprocess.run(
            command + ["--no-record"], env=env, capture_output=True, text=True
        )
        assert result.returncode == code, result.stdout + result.stderr
        assert snapshot(root) == before, (
            "nonrecording gate changed its inputs or benchmark home"
        )
        output = next(
            line[5:] for line in result.stdout.splitlines() if line.startswith("out: ")
        )
        assert not Path(output).exists(), "temporary solver output survived the gate"
    # Recording remains the default, including for a failed gate.
    result = subprocess.run(command, env=env, capture_output=True, text=True)
    assert result.returncode == 1, result.stdout + result.stderr
    assert list((home / "gates").glob("*.yaml")), "recording gate omitted its record"
    assert list((home / "results").glob("*/stats.csv")), (
        "recording gate omitted results"
    )
