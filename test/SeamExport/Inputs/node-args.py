# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Print "op sort: argsort/bits ..." for every term of the export JSON on
# stdin (a walk of the roots, children first) whose op is one of argv[1:].
import json
import sys

ops = set(sys.argv[1:])


def walk(t):
    for a in t.get("args", []):
        walk(a)
    if t["op"] in ops:
        args = " ".join(f"{a['sort']}/{a.get('bits')}" for a in t["args"])
        print(f"{t['op']} {t['sort']}: {args}")


for r in json.load(sys.stdin)["roots"]:
    walk(r["term"])
