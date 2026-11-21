# The A-INB discharge job

`lib/Transforms/CheckInb.cpp` (`sea-check-inb`),
`lib/Transforms/SplitAsserts.cpp` (`sea-split-asserts`), and the driver
`tools/sea-vc/CheckInbJob.h` (`sea-vc --check-inb`). Tests:
`test/Transforms/check-inb.mlir`, `test/Transforms/split-asserts.mlir`,
`test/VC/check-inb.mlir`, and the job lines of
`test/VC/seam/inb-circular.mlir` and `test/VC/seam/inb-ptrmem.mlir`.

Status: first version. Static discharge has one tier; parts run one after
the other.

## Why

An answer of `sea-vc --seam` rests on the pointer premises A-SEP, A-NULL
and A-NOWRAP (SeaM.md). The `assume.*` lines say which premises an answer
used; nothing checked them. A-INB, "every access lies in the object its
pointer came from", implies all three. This job checks A-INB on every
execution of a program within the bound. An unsat of the job, together
with an unsat of the original job under the premises, gives the claim
without them.

## How to run it

```bash
sea-vc p.mlir --check-inb --lower --seam --ackermannize=off \
    --solver=yices --timeout=60
```

The input is an abstract program (`!sea.ptr`). The job:

1. inserts a bounds check before every access (`sea-check-inb`), drops
   the program's own asserts and keeps its assumes;
2. removes the checks it proves with constants (static discharge, below);
3. makes one program per check left (`sea-split-asserts`): part i keeps
   check i and assumes every other check on the paths to it;
4. runs each part as an ordinary safety job, in a process of its own,
   with the options of the command line.

Every part prints one line, then the job prints a summary and the verdict:

```text
check-inb: checks=5 static=3 left=2 zero-length=0
check-inb.part: 0 id=1 at=p.mlir:50:9 result=unsat status=0 time=0.024 solve-time=0.000
check-inb.part: 1 id=3 at=p.mlir:53:9 result=sat status=0 time=0.035 solve-time=0.000 fail-at=p.mlir:53:9
check-inb.parts: total=2 unsat=1 sat=1 unknown=0 other=0 skipped=0 median-time=0.029 max-time=0.035 time=0.061
check-inb.part-diags: none
check-inb.discharges: none under=addr0-is-null
result: sat
```

- `checks`: the access spans checked (a memmove has two); `static`: the
  checks proved by static discharge, never solved; `left`: the parts;
  `zero-length`: ranges of constant length 0, which are not accesses.
- `id`: the check's id (`sea.inb_check`, program order, statically
  discharged checks included); `at`: the access; `fail-at`: the failing
  check of a replayed counterexample; `status`: the part's exit status.
- `check-inb.part-diags`: the warning and error kinds on the parts'
  stderr, each with the number of parts that reported it. A part's
  warnings name approximations and assumptions its answer rests on (for
  example `place.symbolic-size-assume`: an allocation of symbolic size
  gets a fixed room); its `assume.*` lines are in its output
  (`--check-inb-dir`).
- `result: unsat` iff every part is unsat. `sat` if some part's
  counterexample replays: an execution reaches that access out of its
  object, every earlier check holding. Otherwise `unknown` (exit 3), or
  the worst error status of a part (1, 2, 4, 5 or 6). The two can meet:
  with one sat part and one part that failed, the job prints
  `result: sat` and exits with the failed part's status. A failed part's
  line ends with `error=<kind>`, and `check-inb.part-error` gives its
  first error line.
- `check-inb.discharges: a-inb a-sep a-null a-nowrap` comes with
  `result: unsat`: every access of every execution within the bound stays
  in its object, so the premises of `--seam` hold. `a-nowrap` is listed
  only when every object is smaller than 2^(w-1) units (below).
  `under=` names the assumptions the result itself rests on, sorted, or
  `none`: the union over the parts of their `assume.*` lines in effect
  (a nonzero count or `vc`), without the premises the job discharges,
  and of their warnings of a kind ending in `-assume` (an assumption made
  explicit in the program, such as `place.symbolic-size-assume`: an
  allocation of symbolic size is assumed to fit its room). A part's use
  of A-NOWRAP stays in `under=` when `a-nowrap` is not discharged. A
  premise ledger reads it as "discharged by `--check-inb` under ...". A premise ledger can record them as "discharged by
  `--check-inb`". After any other verdict the line says `none`.

`--check-inb` requires `--lower`, `--seam` and `--ackermannize=off`. The
Ackermann lowering uses A-INB itself (to drop constraints of two reads,
SeaM.md); with it off, the premises a part uses rest only on its own
assumes. `--check-inb-dir=<dir>` keeps every part (`part-<i>.mlir`) and
its output (`part-<i>.jsonl`, `part-<i>.err`); `--check-inb-static=false`
turns the static discharge off; `--check-inb-max-time=<s>` starts no part
after that many seconds (the rest are `skipped`, the verdict `unknown`).
Write each option of the command line as `--name=value`: the parts get the
command line without its input file and the job's own options, filtered
token by token. A two-token option (`--program x`) leaves its value
behind, and the parts then fail with status 1. The contents of
`--config` files and `@file` response files reach the parts unfiltered;
each part also gets `--check-inb=false` last, so a `--check-inb` there
does not run the job again inside a part.

## The check

Before every load and store (k units: the width of the value), memset
and memhavoc (n units), and memmove (n units, destination and source),
`sea-check-inb` inserts

```mlir
%ok = sea.ptr.dereferenceable %p, %n
sea.assert %ok {sea.inb_check = <id>}
```

`sea-lower-fat-pointers` lowers it to `p != null && off <=u size &&
n <=u size - off`, with `off` and `size` the offset and object-size
companions of `p` (SeaHorn's fat pointers).

The comparisons are unsigned, at the pointer width w. `off` is the sum of
the pointer arithmetic steps modulo 2^w, as the address is; an offset that
went below the object reads as a large unsigned number and fails
`off <=u size`. `size - off` cannot wrap: it is computed only where
`off <=u size` holds (the conjunction is a single term, but its value
does not depend on `size - off` when `off <=u size` is false). So the check
says: `[off, off + n)` lies within `[0, size)`, as integers.

A range of constant length 0 is not an access and gets no check. A range
of symbolic length n gets `n == 0 || deref(p, n)`. A memhavoc of the null
pointer itself is a no-op by default (the open decision of
`sea-guard-null-memhavoc`, `--memhavoc-null=noop`), so its check also
passes when `p` is null; with `--memhavoc-null=error` it is an access like
any other.

The object of an access is its pointer's provenance, not whatever object
the address lands in. A pointer from `ptr.from_int`, `ptr.null`,
`func_addr` or `ptr.nondet` has an object of size 0, and every access of
positive length through it fails its check. This is stricter than LLVM,
on purpose: a program that reads memory-mapped registers through an
integer cannot be discharged this way. The pass warns
(`check-inb.size0-object`) when an access's pointer comes from such an
op through `ptr.add` and `ptr.index`.

The program's own asserts are dropped: the job checks memory safety, not
the program's property. Its assumes stay.

The job lowers boxes first: `sea-apply-tracking`, then `sea-lower-box-all`
(with `--box-all-expand`), as `--lower` starts. The loads of a
`sea.box.all` body then sit at the box's place, at one arbitrary index of
the box (a nondet `t` with `assume t <u N`), or at every index for an
expanded box, and each gets an ordinary check: a part is sat iff some
index of the box reads out of bounds. A box that may be empty runs its
body only when it is not. The claim's own assert is dropped like any
other. A check at the box's place is stricter than one at the assert: a
read is checked even where an earlier conjunct of the claim already
fails. A discharge check may be stricter. The pass alone refuses a box
that is still there (`check-inb.unsupported`): its loads would go
unchecked while `--seam` rests on A-SEP for them. A box not in assert
polarity is refused by `sea-lower-box-all`, as in `--lower`.

The pass runs on the abstract program before the lowering, so
`sea-drop-fresh-memhavoc` and `sea-guard-null-memhavoc` see the checks and
cannot remove an access before it is checked.

## Static discharge

Tier (a): an access of constant length through a pointer with a constant
offset into an object of constant size (an alloca or malloc of constant
size, or a global, reached through `ptr.add` of constant offsets and
`ptr.index` of constant indices) gets no assert when the check holds for
those constants. The comparison is the one above, on the same modular
offset. The pass never removes a check it has not proved, and the verdict
does not depend on it (`static=false` / `--check-inb-static=false` keeps
every check).

Two more tiers are planned and not built: reuse a dominating check of the
same pointer and at least the same width, and use integer ranges of the
indices (`sea-range-fold`) against a constant size.

## Splitting: one query per check

`sea-split-asserts` is the counterpart of `sea-merge-asserts` and is not
specific to this job. For a program with k asserts, part i keeps assert i
and turns every other assert into an assume of its condition. Part i ends
right after assert i and keeps only assert i's block and its CFG
ancestors; a conditional branch out of that cone becomes an assume of the
condition that stays in it. An assert that no path reaches gives a part
`assert true`; an assert on a CFG cycle is an error. Part i fails iff an
execution reaches assert i with its condition false, every assert it
passed holding. So the program fails some assert iff some part fails, and
it is safe iff every part is.

This is the assume-guarantee structure the job needs. A part's query uses
the premises of the accesses before its check, each of which passed its
own check (an assume in this part). A rewrite under A-SEP of a load before
the check is justified by those checks. The test `@ag` of
`test/VC/check-inb.mlir` pins it: its second access is in bounds only if
the first is, so the second part is unsat only because it assumes the
first check (`@ag_alone`, the second access alone, is sat).

This is also why the job avoids the circularity of the earlier attempts
(SeaM.md, "Using the A-INB-optimized encoding to prove A-INB is
circular"). Those built a predicate "some access leaves its object" from
the encoding under A-INB, so a rewrite of a later load could remove the
violating execution. Here each check is an assert in the program, placed
before its access: an execution that violates A-INB fails the first
check on its path, and the part of that check contains nothing after it.
`inb-circular.mlir` and `inb-ptrmem.mlir`, which broke the earlier
attempts, give `result: sat` at the out-of-bounds store.

[HYP] The argument needs every premise use in a part to be about accesses
before the part's check. Rules that use the A-INB of a later access do not
apply: the part has no later access. The Ackermann lowering is the known
user of A-INB itself, and the job refuses it.

Each part runs as its own sea-vc process. sea-vc's safety flow keeps
global state (the output, the warning summary, the assumption ledger)
that each part needs fresh, and a process per part bounds its memory
(`--mem-limit`) and allows running parts in parallel later.

## A-INB and the premises

With placement's disjoint objects, A-INB of every access implies A-SEP and
A-NULL (SeaM.md). It implies A-NOWRAP for accesses at offsets below
2^(w-1): A-NOWRAP reads the offset as a signed number, and an in-bounds
offset of 2^(w-1) or more reads as negative. Only an object of 2^(w-1)
units or more has such offsets. So the job lists `a-nowrap` among the
premises an unsat discharges only when every placed object is smaller
than 2^(w-1) units, w the pointer width: the largest object the parts'
placements print (`object:` lines), or, when every check was discharged
statically and no part ran, the program's constant object sizes (an
allocation of non-constant size then counts as too large). Otherwise it
warns `check-inb.nowrap-not-implied` with the reason and leaves
`a-nowrap` out (`test/VC/check-inb-nowrap.mlir`: one object of 2^31
units at 32 bits). At 64 bits no placed object is that large.

The two premises also read an access differently. A-INB reads the whole
span of the access, `[off, off + n)`. The A-NOWRAP of the C++ rules
(SeaM.md; used by the stride rules) reads only its start address,
`obj + off`. A-INB is the stronger of the two readings, so its discharge
covers both.

## Not done

- Tiers (b) and (c) of the static discharge.
- Grouping parts, running them in parallel, and stopping at the first
  failure.
- The Ackermann lowering in the job.
- A combined report of this job and the original one.
