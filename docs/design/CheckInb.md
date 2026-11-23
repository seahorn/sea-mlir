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
sea-vc p.mlir --check-inb --lower --seam --ackermannize=off --timeout=60
```

The input is an abstract program (`!sea.ptr`). The job:

1. inserts a bounds check before every access (`sea-check-inb`), drops
   the program's own asserts and keeps its assumes;
2. removes the checks it proves with constants (static discharge, below);
3. makes one program per check left (`sea-split-asserts`): part i keeps
   check i and assumes every other check on the paths to it;
4. runs each part as an ordinary safety job, in a process of its own,
   with the options of the command line (the solver: see below).

Every part prints one line, then the job prints a summary and the verdict:

```text
check-inb: checks=5 static=3 left=2 zero-length=0
check-inb.solver: yices (default) ro-arrays=array (default)
check-inb.split: folded=0 parts=2 trivial=0 to-solve=2
check-inb.part: 0 id=1 at=p.mlir:50:9 result=unsat status=0 time=0.024 solve-time=0.000
check-inb.part: 1 id=3 at=p.mlir:53:9 result=sat status=0 time=0.035 solve-time=0.000 fail-at=p.mlir:53:9
check-inb.parts: total=2 unsat=1 sat=1 unknown=0 other=0 skipped=0 premise=0 median-time=0.029 max-time=0.035 solve-time=0.000 time=0.061
check-inb.part-diags: none
check-inb.discharges: none under=addr0-is-null
result: sat
```

- `check-inb.solver`: the parts' solver and why (`default`, `--solver`,
  or `no yices in this build`), and the read-only array mode and why.
- `checks`: the access spans checked (a memmove has two); `static`: the
  checks proved by static discharge, never solved; `left`: the checks
  left after it; `zero-length`: ranges of constant length 0, which are
  not accesses. `check-inb.split`: `folded`, the checks the optimize
  stages removed before the split; `parts`; `trivial`, the parts whose
  check folded to true in their own optimize (not run, unsat);
  `to-solve`, the parts run. `check-inb.parts` counts the parts run;
  its `solve-time` sums their solver time, `time` is the wall time of the
  runs (processes, parsing and encoding included).
- `id`: the check's id (`sea.inb_check`, program order, statically
  discharged checks included); `at`: the access; `fail-at`: the failing
  check of a replayed counterexample (the replay's `op`, its
  `assert-op`; its `at` when there is none); `status`: the part's exit
  status.
- `check-inb.part-diags`: the warning and error kinds on the parts'
  stderr, each with the number of parts that reported it. A part's
  warnings name approximations and assumptions its answer rests on; its
  `assume.*` lines are in its output (`--check-inb-dir`). The lowering
  runs in the job itself, so its warnings (such as
  `place.symbolic-size-assume`: an allocation of symbolic size gets a
  fixed room) are the job's own, on its stderr.
- `result: unsat` iff every part is unsat. `sat` if some part's
  counterexample replays: an execution reaches that access out of its
  object, every earlier check holding. Otherwise `unknown` (exit 3), or
  the worst error status of a part (1, 2, 4, 5 or 6). The two can meet:
  with one sat part and one part that failed, the job prints
  `result: sat` and exits with the failed part's status. A failed part's
  line ends with `error=<kind>`, and `check-inb.part-error` gives its
  first error line.
- A failed premise is the answer (`--symbolic-size=assert`): a part whose
  run ends with `premise.failed` (status 4) is `result=premise-failed`,
  counted as `premise=` (not `other`), and its `premise.failed:` line
  follows the part's line, with `part=<i>`, naming the allocation, its
  size and its room. The job then prints `result: sat` and exits 4, the
  premise class of a single run; with a sat part as well, the same (the
  rule above: sat, the failed part's status). Nothing is discharged
  (`check-inb.discharges: none`).
- `check-inb.discharges: a-inb a-sep a-null a-nowrap` comes with
  `result: unsat`: every access of every execution within the bound stays
  in its object, so the premises of `--seam` hold. `a-nowrap` is listed
  only when every object is smaller than 2^(w-1) units (below).
  `under=` names the assumptions the result itself rests on, sorted, or
  `none`. It is the union of: the parts' `assume.*` lines in effect (a
  nonzero count or the state word `vc`), without the premises the job
  discharges (`a-sep`, `a-null`, and `a-nowrap` when it is listed);
  `addr0-is-null`, since the job's lowering places null at address 0; the
  job's own warnings of a kind ending in `-assume` (an assumption made
  explicit in the program, such as `place.symbolic-size-assume`: an
  allocation of symbolic size is assumed to fit its room);
  `symbolic-size-bound` when placement bounded such an allocation by an
  assume (counted on the program, so also when no part is left); and
  `memhavoc-null-noop` when a check passes for the null pointer itself
  (`sea-check-inb`'s `null-noop` count) or the lowering guarded or dropped
  a memhavoc whose pointer may be null, even when that check folds away
  and no part reports it. A
  premise ledger reads the line as "discharged by `--check-inb` under
  ...". After any other verdict the line is `none under=<...>`.

`--check-inb` requires `--lower`, `--seam` and `--ackermannize=off`. The
Ackermann lowering uses A-INB itself (to drop constraints of two reads,
SeaM.md); with it off, the premises a part uses rest only on its own
assumes. `--check-inb-dir=<dir>` keeps every part (`part-<i>.mlir`) and
its output (`part-<i>.jsonl`, `part-<i>.err`); `--check-inb-static=false`
turns the static discharge off; `--check-inb-max-time=<s>` starts no part
after that many seconds (the rest are `skipped`, the verdict `unknown`);
`--check-inb-keep` keeps the temporary job directory. A directory that
stays (given, kept, or with `--vcgen-only`) is printed as
`check-inb-dir: <path>`. With `--vcgen-only` the job writes every part
and `manifest.json`, runs no part, and prints `check-inb.manifest:
<path> parts=<k>` and `result: none`: another runner solves the parts
(below).

The parts' options are the job's command line expanded: `--config`
files and `@file` response files read, each option one
`--name[=value]` token (a two-token option such as `--program x`
joined), the input file and the job's own options dropped, and so are
`--lower` and the options that need it (a part is lowered already).
Diagnostic output options do not pass to the parts either (`--log-file`,
`--info-output-file`, `--mlir-pass-pipeline-crash-reproducer`,
`--mlir-print-ir-tree-dir`, `--crash-diagnostics-dir`): they only serve
debugging, and a path in them would tie the job directory to one place;
add one when running a part. The options go into `parts.cfg` in the job
directory, with the pointer mode the job resolved on the input
(`--ptr-mode`). An option that writes one output every part would share
(`--smt2`, `--print-lowered`, `--seam-trail`, `--ack-dump-pairs`,
`--ack-relax-smt2`, `--split-smt2`) is refused when it is in effect
(`check-inb.unsupported`; a value `false`, `0` or empty turns it off):
run it on one part instead. `--ptr-companion=check` is refused with the
other check jobs: it replaces the property of a query (the guard of every
split pointer load, not the program's assertions), so a part would no
longer check its bounds check.

## The parts' solver

The job's own options go to every part, with two defaults of the job:

- Solver: Yices when the binary has it, else z3 (release binaries before
  v0.5.0 are built without Yices), said on the `check-inb.solver:` line
  (`yices (default)`, `z3 (no yices in this build)`, `z3 (--solver)`). An
  explicit `--solver=` on the job's command line (or in its profile) wins
  and is passed through, whatever it is.
- A z3-only option (`--z3-tactic`, `--z3-logic`, `--z3-param`,
  `--z3-plant-error`, `--seam-check-rlimit`) without `--solver` keeps z3
  for the parts (a neutral value does not count: `--z3-tactic=default`,
  an empty `--z3-logic=` or `--z3-param=`, `--z3-plant-error=false`,
  `--seam-check-rlimit=0`); the line gives the option as the reason
  (`z3 (--z3-tactic)`). With an explicit `--solver=yices` or `bitwuzla` the
  option is ignored by the parts' verdict solver (`--z3-param` sets global
  z3 parameters, which the auxiliary z3 checks of a part may still see),
  and the job warns once
  (`check-inb.z3-option-ignored`; `--z3-logic` with another solver is an
  error of sea-vc as everywhere).
- The job's lowering follows the parts' solver, not the job's own: the
  choice that depends on the solver is mul overflow (`sea.umulo` /
  `sea.smulo` stay for z3 and Bitwuzla, are expanded for Yices), decided
  after the parts' solver. The other solver-dependent choices (the
  Ackermann auto mode, `--seam-check` limits) are made inside each part's
  own sea-vc process, which knows its solver.
- With z3 as the parts' solver, `--ro-arrays=uf` is passed unless the
  command line gave `--ro-arrays=`. The option is exact: it states the
  read-only arrays as uninterpreted functions at the Expr level, for any
  backend (`docs/design/Ackermann.md`, "Read-only arrays as functions").
  It is passed for z3 only, where it helped; for Yices and Bitwuzla it is
  not passed (they were not measured with it).
  The read-only arrays then are functions in the parts' queries: with
  `--z3-logic`, a logic without `UF` (`QF_BV`, `QF_ABV`) makes such a part
  an error (`vc.z3-logic-mismatch`, the job `unknown`); use `QF_UFBV` /
  `QF_AUFBV` or `--ro-arrays=array`.

Measured (`h2fc-blowup-tx-reach-ring.mlir`, `--check-inb --lower --seam
--widen-memory=4 --ackermannize=off --timeout=300`, 141 parts): z3 took
more than 1200 s for 101 parts; Yices 336 s for all 141 (120 unsat, 21
sat). Part 27: z3 18.8 s, z3 with `--ro-arrays=uf` 1.48 s, Yices 0.09 s.
The defaults are those measurements, not tuned further. The chosen options
are in `parts.cfg` and each part's manifest argv.

## The manifest

The job runs in two steps. First it writes every part
(`part-<i>.mlir`), the parts' options (`parts.cfg`) and `manifest.json`
into the job directory; then it runs the parts, each with exactly the
manifest's argv (argv[0] its own binary), in the job directory. A runner
outside sea-vc (distributed, threaded, in one process) can take the
directory and run the parts its own way; the manifest is enough.

The directory is self-contained and can be moved: every path in the
manifest is relative to the manifest's directory, and a part runs with
that directory as its working directory. `--config=parts.cfg` names a
file with an extension, which sea-vc reads from the working directory
(only a bare name, with no `/` and no extension, is looked up in the
profile directories). argv[0] is `sea-vc`: a runner puts its own binary
there (`sea_vc` is a hint, the binary that wrote the job). The source
file is provenance only; running a part does not read it.

`manifest.json`, format version 1. Fields are only ever added; a reader
ignores fields it does not know and checks `format` and `version`.

| field | meaning |
|---|---|
| `format` | `"sea-vc-check-inb"` |
| `version` | the format version, `1` |
| `sea_vc_version` | the `tool.version` of the sea-vc that wrote it |
| `sea_vc` | the absolute path of that sea-vc (a hint) |
| `cwd` | `"."`: a part runs in the manifest's directory |
| `config` | `"parts.cfg"`: the parts' options (sea-vc config-file syntax, one option per line) |
| `source` | `{path, sha256}`: the input file (absolute path) and the SHA-256 of its contents (`null` for standard input); provenance only |
| `program` | the program's name |
| `args` | the job's command line, without the binary |
| `counters` | `{checks, static, left, zero_length}`, as on the `check-inb:` line, and `{folded, parts, trivial}`, as on `check-inb.split:` |
| `parts` | one object per part that needs a solver, in order (a trivial part, whose assert folded to true, is counted, not listed) |
| `discharge` | the job-level inputs of `check-inb.discharges` (below): `pointer_width`; `largest_object` (decimal string, units: the largest placed object, null aside); `nowrap_implied` (`largest_object` < 2^(pointer_width-1)); `null_noop` (checks that pass for memhavoc(null), plus memhavocs the lowering guarded or dropped for it); `under` (the job's own contributions to `under=`, sorted: `addr0-is-null`, `memhavoc-null-noop` when `null_noop` > 0, the job's `*-assume` warning kinds) |

Each part:

| field | meaning |
|---|---|
| `index` | the part's number, as on `check-inb.part:` |
| `check_id` | the check's id (`sea.inb_check`) |
| `at` | the check's source location (the access) |
| `mlir` | the part's file, relative to the job directory |
| `argv` | the command line that runs it in the job directory: `sea-vc`, the part's file, `--config=parts.cfg`, `--format=jsonl`, `--check-inb=false` (never `--vcgen-only`, which the job does not pass on) |
| `stdout`, `stderr` | the file names (in the job directory) the job's own runner writes the part's output to; the job reads `result`, `solve-time` and the replay's `op` (else `at`) from the jsonl |

A part's verdict is the `result` record of its jsonl and its exit status
(the statuses of `sea-vc --agent`). The job's verdict is unsat iff every
part is unsat.

### Rebuilding the discharges line

An outside runner rebuilds `check-inb.discharges` from the manifest and
the parts' outputs:

- premises: if every listed part's `result` is `unsat` and it exited
  with status 0 (no part listed: vacuously), `a-inb a-sep a-null`, plus
  ` a-nowrap` when
  `discharge.nowrap_implied`; otherwise `none`;
- under: the union of `discharge.under`; over the parts, the names
  `<n>` of their `assume.<n>` stat records in effect (some field other
  than `kind`, `key`, `text` is a nonzero number or a nonempty string
  other than `0`), except `a-sep` and `a-null`, and except `a-nowrap`
  when the premises list it; and the kinds `[<kind>]` ending in
  `-assume` of their stderr `Warning:` and `Error:` lines. Sorted,
  comma-separated; `none` if empty;
- the line: `check-inb.discharges: <premises> under=<under>`.

`test/VC/Inputs/rebuild-discharges.py` does this, and
`test/VC/check-inb-rebuild.mlir` checks it against the job's own line.

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
the program's property. Its assumes stay, and so do premise asserts
(`sea.premise`). With `--symbolic-size=assert`, placement (which runs
after the instrumentation) makes each bound of a symbolic allocation size
a premise assert, and the split makes it a part of its own (`id=-1`): a
part that fails it ends with `premise.failed` (status 4): the job's
answer, in the premise class (above), not a bounds violation.

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

Each part runs as its own sea-vc process, on the lowered part: it encodes
and solves, and does not lower again. sea-vc's safety flow keeps global
state (the output, the warning summary, the assumption ledger) that each
part needs fresh, and a process per part bounds its memory
(`--mem-limit`) and allows running parts in parallel later.

## Instrument, optimize, lower, unique

Instrumentation, optimization and assertion uniquing (merging or
splitting) are three independent steps. `--lower` combines them one way:
it lowers and then merges every assert into one. The job combines them
differently:

```text
instrument -> optimize -> lower -> optimize -> unique -> optimize each part
```

1. *Instrument*: `sea-apply-tracking`, `sea-lower-box-all`, then
   `sea-check-inb` on the abstract program (static discharge included).
2. *Optimize*: `sccp`, `cse`, `sea-assume-fold`, `canonicalize`, `cse`:
   the folds of `--lower` that are exact with many asserts in the
   program. A check that an earlier check (or assume) implies on every
   path folds to `assert true` and disappears. This is the BMC reading:
   the merged program fails at the first failing assert on a path, so a
   later assert may assume the earlier ones, the same assume-guarantee
   step the split takes. Most checks go here, before any part exists.
3. *Lower*: the memory lowering of `--lower`, once (`sea-drop-fresh-memhavoc`,
   `sea-guard-null-memhavoc`, `sea-lower-tracking`, `sea-lower-fat-pointers`,
   `sea-place-static`, widening and the memory-operation expansions). The
   checks become offset and size arithmetic; placement happens once for
   every part.
4. *Optimize* again: placement made addresses constant.
5. *Unique*: `sea-split-asserts` (or `sea-merge-asserts`; or, a hook not
   built yet, groups of g asserts merged within each group: the split's
   `group` parameter, 1 today).
6. *Optimize each part*, with the rest of `--lower`: `sea-assume-fold`,
   `sea-merge-asserts` (one assert: a no-op), then the tail
   (`sea-split-wide-access`, `sea-expand-intrinsics`, `canonicalize`,
   `cse`, `sea-prune-edges`, `sea-split-critical-edges`). Inside a part
   the earlier checks are assumes, and only its cone is left, so folds
   blocked before (a possibly unreachable access) may go through. A part
   whose assert folds to true needs no solver.

`lib/Transforms/Passes.cpp` assembles `--lower` from the same stages
(prefix, memory lowering, assume-fold, merge, tail), in the same order,
so its queries do not change. The parts are lowered programs in BMC
form: each part's argv runs sea-vc on it without `--lower` (and without
the options that need `--lower`), with the pointer mode the job resolved
on the input (`--ptr-mode`), so a part does not lower or place again.
What the lowering assumed is the job's to report: `under=` adds the
lowering's own facts (null is address 0, the explicit `*-assume`
warnings, memhavoc(null) as a no-op: checks that pass for null and
memhavocs guarded or dropped for it) to the parts' lines.

Counters: `static` (tier (a)), `folded` (checks removed by the optimize
stages before the split), `parts`, `trivial` (a part whose assert
folded to true after its own optimize: not run, unsat), `to-solve`
(the parts run), and `solve-time` (the parts' solver time, summed)
apart from `time` (wall time of the runs, mostly process and encoding
overhead).

## A-INB and the premises

With placement's disjoint objects, A-INB of every access implies A-SEP and
A-NULL (SeaM.md). It implies A-NOWRAP for accesses at offsets below
2^(w-1): A-NOWRAP reads the offset as a signed number, and an in-bounds
offset of 2^(w-1) or more reads as negative. Only an object of 2^(w-1)
units or more has such offsets. So the job lists `a-nowrap` among the
premises an unsat discharges only when every object is smaller than
2^(w-1) units, w the pointer width. `sea-check-inb` records the size of
the program's largest object as placement sizes it
(`sea.check_inb_largest_object`): a constant size, or for an allocation
of non-constant size the room placement reserves for it (4096 units,
`kPlaceMaxSymbolicSize`; the size is assumed to fit, and `under=` says
so). The program is the one after the box lowering and before the
split, so every part's objects are among its objects. Otherwise the job
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
