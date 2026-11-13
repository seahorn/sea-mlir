#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Checks a tool's --format=jsonl output against its --format=text output.

  jsonl-check.py --text T --jsonl J [--require KIND[:FIELD,...]]...

1. Every line of J is one JSON object with a string "kind" and "text".
2. The "text" fields, one per line, are T (timings masked: two runs;
   also time=<s>, execs/s=<n> and steps/s=<n> fields, and the memory
   sizes rss-mb=, vm-mb=, peak-rss-mb= of the --mem-limit notes).
3. Records with a fixed rendering are rendered from their fields and must
   equal their "text": stat (key: value k=v ...; BRUNCH_STAT), program,
   result, nondet, memread, validate, object, print.object, print
   (lines mode), print.mem, print.mem.row, replay, status.
4. Each --require KIND[:F1,F2] has a record of that kind with those fields.

Exit 0 when all hold; else the first failures on stderr and exit 1.
"""

import argparse
import json
import re
import sys

TIME = re.compile(r"(\S*time\S*:? )-?[0-9]+\.[0-9]+")
TIME_KV = re.compile(r"\b((?:time|execs/s|steps/s)=)-?[0-9]+(?:\.[0-9]+)?")
MEM_KV = re.compile(r"(?<![\w-])((?:rss|vm|peak-rss)-mb=)[0-9]+")


def mask(s):
    return MEM_KV.sub(r"\1M", TIME_KV.sub(r"\1T", TIME.sub(r"\1T", s)))


def raw(v):
    """A field value as text prints it (numbers as written)."""
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def inlined_at(sites):
    """` inlined-at=<site>,...` of a call-site array; "" for none."""
    if not sites:
        return ""
    if not isinstance(sites, list) or not all(isinstance(x, str) for x in sites):
        return " <bad inlined field>"
    return " inlined-at=" + ",".join(sites)


def print_locs(f):
    """A print or print.object record's ` at=.. [inlined-at=..]
    [at_name=..]` (empty without at)."""
    s = ""
    if "at" in f:
        s += " at=" + f["at"] + inlined_at(f.get("at_inlined"))
    if "at_name" in f:
        s += " at_name=" + f["at_name"]
    return s


def render(r):
    """The text of record r from its fields, or None (no fixed rendering)."""
    k = r["kind"]
    f = {key: v for key, v in r.items() if key not in ("kind", "text")}
    if k == "stat":
        if f.pop("brunch", False):
            return "BRUNCH_STAT %s %s" % (f["key"], raw(f["value"]))
        key = f.pop("key")
        parts = []
        if "value" in f:
            parts.append(raw(f.pop("value")))
        for name, v in f.items():
            parts.append("%s=%s" % (name.rstrip("_"), raw(v)))
        return key + ":" + ("" if not parts else " " + " ".join(parts))
    if k == "program":
        return "program: @" + f["name"]
    if k == "result":
        return "result: " + f["value"]
    if k == "nondet":
        s = "nondet: site=%s callee=%s width=%s value=%s" % (
            f["site"],
            f["callee"],
            f["width"],
            raw(f["value"]),
        )
        return s + (" origin=" + f["origin"] if "origin" in f else "")
    if k == "memread":
        # an abstract memory's cell: obj + off, no addr (sea-run)
        where = raw(f["addr"]) if "addr" in f else "%s+%s" % (f["obj"], raw(f["off"]))
        s = "memread: site=%s addr=%s width=%s value=%s" % (
            f["site"],
            where,
            f["width"],
            raw(f["value"]),
        )
        if "origin" in f:
            s += " origin=" + f["origin"]
        if "addr" in f and "obj" in f:
            s += " obj=&%s[%s]" % (f["obj"], raw(f["off"]))
        return s
    if k == "validate":
        return "validate: checked=%s mismatches=%s dontcare=%s" % (
            f["checked"],
            f["mismatches"],
            f["dontcare"],
        )
    if k == "object":
        if f["symbol"] != "obj." + f["id"]:
            return "<symbol %s is not obj.<id>>" % f["symbol"]
        return "object: name=%s order=%s kind=%s base=%s size=%s align=%s" % (
            f["symbol"],
            f["order"],
            f["objkind"],
            raw(f["base"]),
            raw(f["size"]),
            f["align"],
        )
    if k == "print.object":
        s = "print.object: %s kind=%s size=%s" % (f["id"], f["objkind"], raw(f["size"]))
        if "label" in f:
            s += " label=" + f["label"]
        s += print_locs(f)
        if "base" in f:
            s += " base=%s" % hex(int(f["base"]))
        return s + (" freed" if f.get("freed") else "")
    if k in ("print", "print.mem") and "depth" not in f:
        s = "print: " + f["path"]
        if "repr" in f:
            s += " = " + f["repr"]
        elif "error" in f:
            return None  # <dangling: x> / <stale: ...>: free text
        return s
    if k == "print.mem.row" and "depth" not in f:
        return "print: %s %s" % (f["path"], f["row"])
    if k in ("replay", "status"):
        p = "replay: " if k == "replay" else ""
        lines = ["%s: %s" % (k, f["status"])]

        def where(key):
            # a position (`-` without one), then ` inlined-at=<sites>` if
            # inlined, then ` name=<name>` if named
            s = f.get(key, "-") + inlined_at(f.get(key + "_inlined"))
            if key + "_name" in f:
                s += " name=" + f[key + "_name"]
            return s

        if "at" in f or "at_name" in f:
            lines.append(p + "assert-at: " + where("at"))
        if "loc" in f:
            lines.append("at: " + f["loc"])
        if "op" in f or "op_name" in f:
            lines.append(p + "assert-op: " + where("op"))
        if "box_all_false" in f:
            s = p + "box-all-false: index=" + f["box_all_false"]
            if "box_all_op" in f or "box_all_op_name" in f:
                s += " op=" + where("box_all_op")
            lines.append(s)
        if "path" in f:
            sites = f.get("path_inlined", [[]] * len(f["path"]))
            if len(sites) != len(f["path"]):
                return "<path_inlined is not parallel to path>"
            lines.append(
                p
                + "assert-path: "
                + " <- ".join(pos + inlined_at(c) for pos, c in zip(f["path"], sites))
            )
        if "reached" in f:
            lines.append("reached: %s at=%s" % (f["reached"], where("reached_at")))
        if "message" in f:
            lines.append("message: " + f["message"])
        if "deferred" in f:
            lines.append(
                "deferred: %s kind=%s at=%s"
                % (f["deferred"], f["deferred_kind"], where("deferred_at"))
            )
        if k == "status":
            # sea-run: assert-at comes after at:
            order = [
                "status",
                "at: ",
                "assert-at",
                "assert-op",
                "box-all-false",
                "assert-path",
                "reached",
                "message",
                "deferred",
            ]
            lines.sort(
                key=lambda l: next(i for i, o in enumerate(order) if l.startswith(o))
            )
        return "\n".join(lines)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", required=True)
    ap.add_argument("--jsonl", required=True)
    ap.add_argument("--require", action="append", default=[])
    a = ap.parse_args()
    errors = []
    recs = []
    with open(a.jsonl) as fh:
        for n, line in enumerate(fh.read().splitlines(), 1):
            try:
                # numbers as written (0.000 stays 0.000)
                r = json.loads(line, parse_float=str, parse_int=str)
            except json.JSONDecodeError as e:
                errors.append("line %d: not JSON (%s): %s" % (n, e, line))
                continue
            if (
                not isinstance(r, dict)
                or not isinstance(r.get("kind"), str)
                or not isinstance(r.get("text"), str)
            ):
                errors.append("line %d: no kind/text: %s" % (n, line))
                continue
            recs.append(r)
            want = render(r)
            if (
                want is not None
                and "at" in r
                and r["kind"] != "replay"
                and r["kind"] != "status"
                and r["text"] == want + print_locs(r)
            ):
                want += print_locs(r)  # --print-locs
            if want is not None and want != r["text"]:
                errors.append(
                    "line %d: fields render as\n  %r\nbut text is\n"
                    "  %r" % (n, want, r["text"])
                )
    with open(a.text) as fh:
        text = fh.read()
    joined = "".join(r["text"] + "\n" for r in recs)
    if mask(joined) != mask(text):
        tl, jl = mask(text).splitlines(), mask(joined).splitlines()
        for i in range(max(len(tl), len(jl))):
            x = tl[i] if i < len(tl) else "<eof>"
            y = jl[i] if i < len(jl) else "<eof>"
            if x != y:
                errors.append(
                    "text line %d differs:\n  text:  %r\n  jsonl: %r" % (i + 1, x, y)
                )
                break
    for req in a.require:
        kind, _, fields = req.partition(":")
        need = [x for x in fields.split(",") if x]
        if not any(r["kind"] == kind and all(x in r for x in need) for r in recs):
            errors.append("no %s record with fields %s" % (kind, need))
    for e in errors[:10]:
        print("jsonl-check: " + e, file=sys.stderr)
    if errors:
        return 1
    print("jsonl-check: ok, %d records" % len(recs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
