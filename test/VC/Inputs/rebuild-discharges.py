# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Rebuilds the `check-inb.discharges:` line of a sea-vc --check-inb job
# from its directory alone (manifest.json and the parts' outputs), by the
# rule of docs/design/CheckInb.md ("Rebuilding the discharges line"), and
# compares it with the job's own line.
# Usage: rebuild-discharges.py <job dir> <the job's text output>
import json
import os
import re
import sys

d, job_out = sys.argv[1], sys.argv[2]
with open(os.path.join(d, "manifest.json")) as f:
    m = json.load(f)
dis = m["discharge"]
cwd = os.path.join(d, m["cwd"])

all_unsat = True
part_assumes = set()
part_kinds = set()
for p in m["parts"]:
    result = None
    try:
        with open(os.path.join(cwd, p["stdout"])) as f:
            for line in f:
                r = json.loads(line)
                if r.get("kind") == "result":
                    result = r["value"]
                elif r.get("kind") == "stat" and r.get("key", "").startswith("assume."):
                    active = any(
                        (isinstance(v, (int, float)) and v != 0)
                        or (isinstance(v, str) and v not in ("", "0"))
                        for k, v in r.items()
                        if k not in ("kind", "key", "text")
                    )
                    if active:
                        part_assumes.add(r["key"][len("assume.") :])
    except FileNotFoundError:
        pass
    if result != "unsat":
        all_unsat = False
    try:
        with open(os.path.join(cwd, p["stderr"])) as f:
            for line in f:
                line = line.rstrip()
                mk = re.search(r"\[([^\]]+)\]$", line)
                if line.startswith("Warning:") and mk:
                    part_kinds.add(mk.group(1))
    except FileNotFoundError:
        pass

if all_unsat:
    premises = "a-inb a-sep a-null" + (" a-nowrap" if dis["nowrap_implied"] else "")
else:
    premises = "none"
under = set(dis["under"])
for a in part_assumes:
    if a in ("a-sep", "a-null"):
        continue
    if a == "a-nowrap" and "a-nowrap" in premises:
        continue
    under.add(a)
under |= {k for k in part_kinds if k.endswith("-assume")}
rebuilt = "check-inb.discharges: %s under=%s" % (
    premises,
    ",".join(sorted(under)) if under else "none",
)
with open(job_out) as f:
    job = next(
        (l.rstrip("\n") for l in f if l.startswith("check-inb.discharges:")), None
    )
print(
    "discharges-match"
    if job == rebuilt
    else "MISMATCH job=%r rebuilt=%r" % (job, rebuilt)
)
