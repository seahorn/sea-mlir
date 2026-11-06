# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Joins the records of a sea-vc --format=jsonl run on the object id.

Every object id a record names (print.object id, print.mem obj, print obj /
near, memread obj, mismatch obj) is the id of an `object` record; the
`object` record's symbol is obj.<id>; print.object's base and size equal
the object's, and a memread's addr is the object's base + off. Prints
`join: <n> references` and exits 0 when all hold.
"""

import json
import sys

recs = [json.loads(l) for l in open(sys.argv[1])]
objs = {r["id"]: r for r in recs if r["kind"] == "object"}
assert objs, "no object records"
n = 0
for r in recs:
    k = r["kind"]
    if k == "object":
        assert r["symbol"] == "obj." + r["id"], r
        continue
    for field in ("id", "obj", "near"):
        if field == "id" and k != "print.object":
            continue
        if field not in r or r[field] == "null":
            continue
        o = objs.get(r[field])
        assert o is not None, "unknown object id %r in %s" % (r[field], r)
        n += 1
        if k == "print.object":
            assert int(r["base"]) == int(o["base"]), r
            assert int(r["size"]) == int(o["size"]), r
        if k == "memread":
            assert int(r["addr"]) == int(o["base"]) + int(r["off"]), r
print("join: %d references" % n)
