#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""utils/seavc_flags.py: a failed --help-hidden probe is an error, in the
module and in every tool that compares written queries (no build needed:
fake sea-vc binaries).

usage: seavc-flags.py <utils dir>
"""

import os
import shutil
import stat
import subprocess
import sys
import tempfile

utils = sys.argv[1]
sys.path.insert(0, utils)
import seavc_flags as sf  # noqa: E402

d = tempfile.mkdtemp()


def fake(name, body):
    p = os.path.join(d, name)
    with open(p, "w") as f:
        f.write("#!/bin/sh\n" + body + "\n")
    os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR)
    return p


new = fake("new", "echo '  --vc-fold-top-eqs - fold'")
old = fake("old", "echo '  --seam - SeaM'")
fail = fake("fail", "exit 1")
slow = fake("slow", "exec sleep 10")
missing = os.path.join(d, "missing")

# a binary that has the fold gets the flag, an older one (probe fine) not
assert sf.no_fold(new) == [sf.NO_FOLD]
assert sf.no_fold(old) == []
print("module: probe ok")

# a failed probe raises, never "does not know"
sf.PROBE_TIMEOUT = 1
for b, why in ((fail, "exit 1"), (missing, "No such file"), (slow, "timed out")):
    try:
        sf.no_fold(b)
    except sf.ProbeError as e:
        assert why in str(e), (b, str(e))
        print(f"module: {os.path.basename(b)} raises")
    else:
        raise AssertionError(f"{b}: no ProbeError")

# every caller stops with its error status before any job
tools = [
    ("query-diff.py", ["--old", new, "--new", fail, "x.mlir"], 1),
    ("encoding-diff.py", ["--sea-vc", fail, "l.mlir", "r.mlir"], 1),
    ("cover-diff.py", ["--sea-vc", fail, "--level", "1", "x.mlir"], 3),
    ("memops-equiv.py", ["--sea-vc", fail, "x.mlir"], 2),
]
for tool, args, code in tools:
    p = subprocess.run(
        [sys.executable, os.path.join(utils, tool)] + args,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert p.returncode == code, (tool, p.returncode, p.stdout, p.stderr)
    assert "error: seavc_flags: cannot probe" in p.stderr, (tool, p.stderr)
    assert fail in p.stderr, (tool, p.stderr)
    print(f"tool: {tool} exit={p.returncode}")
shutil.rmtree(d)
print("seavc-flags: ok")
