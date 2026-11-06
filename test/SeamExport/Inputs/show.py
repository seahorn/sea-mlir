#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Test helper: checks the structure of a sea-seam-export document (schema 2,
docs/SeamExport.md) and prints it one record per line, terms as
op(args), so FileCheck can pin the key lines. Exits 1 on a structural error.
"""

import json
import sys

TOP = {
    "schema",
    "lane",
    "sea_vc",
    "ptr_mode",
    "address_bits",
    "ainb",
    "rewrite",
    "roots",
    "side",
    "sources",
    "probes",
    "erased_roots",
    "erased_side",
}
SORTS = {"bv", "bool", "ptr", "memory"}


def term(t, erased):
    if not isinstance(t, dict) or t.get("sort") not in SORTS or "op" not in t:
        raise SystemExit(f"bad term: {t}")
    if erased and t["sort"] == "ptr":
        raise SystemExit("pointer sort in the erased lane")
    if t["sort"] in ("bv", "ptr") and not isinstance(t.get("bits"), int):
        raise SystemExit(f"no width: {t}")
    op = t["op"]
    if op == "bv":
        return t["value"]
    if op == "bool":
        return "true" if t["value"] else "false"
    if op == "var":
        return t["name"]
    args = [term(a, erased) for a in t["args"]]
    if op == "index":
        args += [f"{term(i['value'], erased)}*{i['stride']}" for i in t["indices"]]
        args += [f"c={t['constant']}", f"K={t['byte_offset']}", t["flag"]]
    elif op == "smap":
        args += [f"{e['offset']}:{term(e['value'], erased)}" for e in t["entries"]]
    elif op in ("wselect", "wstore"):
        args += [f"bytes={t['bytes']}", f"align={t['alignment']}"]
    elif op == "extract":
        args += [f"start={t['start']}", f"len={t['length']}"]
    return f"{op}({', '.join(args)})"


d = json.load(sys.stdin)
keys = set(d)
extra = keys - TOP - {"firings", "firings_unexpressible"}
if not TOP <= keys or extra:
    raise SystemExit(f"top-level keys: missing {TOP - keys}, unknown {extra}")
if d["schema"] != 2:
    raise SystemExit("schema")
print(
    f"header: schema={d['schema']} lane={d['lane']} sea_vc='{d['sea_vc']}' "
    f"ainb={str(d['ainb']).lower()} rewrite={str(d['rewrite']).lower()} "
    f"ptr_mode={d['ptr_mode']} address_bits={d['address_bits']}"
)
for r in d["roots"]:
    print(
        f"root {r['name']} {r['operation']} {r['term']['sort']}: {term(r['term'], False)}"
    )
for t in d["side"]:
    print(f"side: {term(t, False)}")
for s in d["sources"]:
    key = f"site={s['site']}" if "site" in s else f"fixed={s['fixed']}"
    print(f"source {key}: {term(s['term'], False)}")
for p in d["probes"]:
    print(
        f"probe {p['location']} bytes={p['bytes']}: "
        f"{term(p['memory'], False)} @ {term(p['address'], False)}"
    )
for r in d["erased_roots"]:
    print(f"erased {r['name']}: {term(r['term'], True)}")
for t in d["erased_side"]:
    print(f"erased-side: {term(t, True)}")
if "firings" in d:
    for f in d["firings"]:
        head = (
            f"firing {f['id']} rules={','.join(f['rules'])} "
            f"premises={','.join(f['premises']) or 'none'} op={f['op']}"
            + (f" units={f['units']}" if "units" in f else "")
        )
        if "unexpressible" in f:
            print(f"{head} unexpressible: {f['unexpressible']}")
            continue
        print(head)
        print(f"  lhs: {term(f['lhs'], False)}")
        print(f"  rhs: {term(f['rhs'], False)}")
        print(f"  premise: {term(f['premise'], False)}")
    print(f"firings: {len(d['firings'])} unexpressible={d['firings_unexpressible']}")
