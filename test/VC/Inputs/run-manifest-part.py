# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Runs one part of a sea-vc --check-inb manifest by hand, as an outside
# runner would: its argv with argv[0] (`sea-vc`) replaced by the given
# binary, the working directory the manifest's directory, stdout and
# stderr to the part's files there. Prints the part's result line.
# Usage: run-manifest-part.py <dir> <index> <sea-vc binary>
import json
import os
import subprocess
import sys

d, k, binary = sys.argv[1], int(sys.argv[2]), sys.argv[3]
with open(os.path.join(d, "manifest.json")) as f:
    m = json.load(f)
part = next(p for p in m["parts"] if p["index"] == k)
cwd = os.path.join(d, m["cwd"])
argv = [binary] + part["argv"][1:]
with (
    open(os.path.join(cwd, part["stdout"]), "w") as out,
    open(os.path.join(cwd, part["stderr"]), "w") as err,
):
    status = subprocess.call(argv, stdout=out, stderr=err, cwd=cwd)
with open(os.path.join(cwd, part["stdout"])) as f:
    for line in f:
        r = json.loads(line)
        if r.get("kind") == "result":
            print(f"part {k} result={r['value']} status={status}")
