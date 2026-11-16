# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Print "op sort bits" once per term op of the export JSON on stdin, in the
# order of first occurrence (a walk of the roots).
import json
import sys

seen = {}


def walk(t):
    for a in t.get("args", []):
        walk(a)
    seen.setdefault(t["op"], (t["sort"], t.get("bits")))


for r in json.load(sys.stdin)["roots"]:
    walk(r["term"])
for op, (sort, bits) in seen.items():
    print(op, sort, bits)
