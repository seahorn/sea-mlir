# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Runs one part of a sea-vc --check-inb manifest by hand, as an outside
# runner would: its argv, stdout to its stdout file in the job directory.
# Prints the part's result line. Usage: run-manifest-part.py <dir> <index>
import json
import os
import subprocess
import sys

d, k = sys.argv[1], int(sys.argv[2])
with open(os.path.join(d, "manifest.json")) as f:
    m = json.load(f)
part = next(p for p in m["parts"] if p["index"] == k)
with (
    open(os.path.join(d, part["stdout"]), "w") as out,
    open(os.path.join(d, part["stderr"]), "w") as err,
):
    status = subprocess.call(part["argv"], stdout=out, stderr=err)
with open(os.path.join(d, part["stdout"])) as f:
    for line in f:
        r = json.loads(line)
        if r.get("kind") == "result":
            print(f"part {k} result={r['value']} status={status}")
