# sea-seam-export: SeaM terms for the Lean conformance checks

`sea-seam-export` prints the SeaM terms of one straight-line `sea.program`
as one JSON document. It is the exporter of the conformance checks of
[seam-lean](https://github.com/agurfinkel/seam-lean), where the SeaM pointer
semantics, its erasure to bit-vectors and the rewrite rules are formalized
in Lean. That formalization is not part of a sea-mlir release; the checks
run from seam-lean, against a pinned sea-mlir revision.

The tool builds nothing itself. Production code builds every term:
`SeaOpSem` (the VC encoding), the `SeaMRewriter` inside it, and
`SeaOpSem::solverTerm` (the solver boundary). The tool prints the terms as
built and re-implements no rule. No solver runs.

## What seam-lean does with it

- **Semantics and erasure.** For a concrete input, the value of every
  register in `sea-run` must equal Lean's evaluation of its raw term, of
  the Lean erasure of that term, and of production's erased term
  (`erased_roots`). The raw lanes give the terms before any rewrite.
- **Rule witnesses.** With `--firings`, every rule application of a run is
  a record `premise -> lhs = rhs`. A Lean checker validates each one as an
  instance of a proved rule.

## Usage

```
sea-seam-export p.mlir [--lane=<lane>] [--firings] [--address-bits=8]
sea-seam-export --agent | --version | --help
```

Input: a module with exactly one `sea.program`. The program has one block
without arguments, ends in `sea.halt`, is placed (`sea.segment`,
`!sea.addr<N>`, N = `--address-bits`), and uses byte memories indexed by
N-bit addresses. The program is taken as given, with no lowering. The pointer
mode is `address` if the program has `sea.addr.from_int`, else `opaque`
(as `sea-vc --ptr-mode=auto` decides).

Status: 0 exported; 1 error, with nothing on stdout (see "Fail closed");
3 exported, but some firings are unexpressible.

## Lanes

A lane is one configuration of `SeaOpSem`. `sea_vc` in the output names
the `sea-vc` command whose VC terms the lane builds. It is empty when no
`sea-vc` run builds them.

| lane | `sea_vc` | what |
|---|---|---|
| `memory-rules0` (default) | | the `--seam --seam-wide` encoding with the memory rules, simplification, the layout decision and A-INB off; the pointer rules run at accesses |
| `seam-wide-inb0-simplify0-layout0` (`--rewrite`) | `sea-vc --seam --seam-wide --seam-inb=0 --seam-simplify=0 --seam-layout=0` | memory rules on |
| `raw-wide` | | the `--seam --seam-wide` encoding before any rule |
| `raw` | | the `--seam` encoding (byte accesses) before any rule |
| `seam-wide-inb0` | `sea-vc --seam --seam-wide --seam-inb=0` | |
| `seam-inb0` | `sea-vc --seam --seam-inb=0` | |
| `seam-inb0-pull-concat` | `sea-vc --seam --seam-inb=0 --seam-pull-concat=4` | split (byte) pointer loads resolved; the only lane where `ptr.load-resolve-concat` can fire |
| `seam-wide` | `sea-vc --seam --seam-wide` | under A-INB (`"ainb": true`) |
| `seam` | `sea-vc --seam` | under A-INB (`"ainb": true`) |
| `plain` | `sea-vc` | no SeaM and no rewriter. The boundary is `PtrIndexExpander` (the lowering's arithmetic) |
| `simplify` | `sea-vc --simplify` | no SeaM: the bit-vector and Boolean rules only |

Notes:
- Each `sea_vc` command sets the same `SeaMOptions` fields as the lane.
  The other fields keep their defaults in both.
- `raw-wide` and `raw` switch the rewriter off (`SeaMOptions::rules =
  false`, set only by this tool). Their terms are the encoding's own.
- Lanes with simplification also normalize the side conditions and the
  registers with `SeaOpSem::simplify`, as `sea-vc` normalizes its side
  conditions and query.
- Lanes without wide accesses (`raw`, `seam-inb0`, `seam-inb0-pull-concat`, `seam`, `plain`,
  `simplify`) refuse a multi-byte access (`vc.wide-access`). `sea-vc
  --lower` would split it.
- An A-INB lane is an error when the program statically violates A-INB.
  `sea-vc` would rerun without A-INB.
- In `plain` and `simplify`, an object is its placed address
  (`fromInt(n)`), not a variable `obj.<name>`.

## Format (schema 2)

One JSON object with sorted keys:

| key | value |
|---|---|
| `schema` | `2`. Additions (new keys, new term ops) keep the number; an incompatible change bumps it |
| `lane`, `sea_vc` | the lane and its `sea-vc` command (see above) |
| `ptr_mode` | `address` or `opaque` |
| `address_bits` | N |
| `ainb` | whether A-INB was used (the A-INB lanes) |
| `rewrite` | whether the memory rules ran |
| `roots` | `[{name, location, operation, term}]`: every `iN`, address and memory result, in program order |
| `side` | `[term]`: the side conditions, such as the layout `addr(obj.<name>) = base` |
| `sources` | `[{site, term}]` for `sea.nondet`, `sea.mem.nondet` and `sea.memhavoc` (the havoc array), and `[{fixed, term}]` for `sea.segment` with its placed base |
| `probes` | `[{location, bytes, memory, address}]`: the operands of each load |
| `erased_roots` | `[{name, location, term}]`: the non-memory roots after `solverTerm` (no pointer sort is left) |
| `erased_side` | `[term]`: the side conditions after `solverTerm` |
| `firings` | (`--firings`) see below |
| `firings_unexpressible` | (`--firings`) how many firings carry `unexpressible` |

Names are the VC's: `nd.<site>_0` for a nondet, `obj.<name>` for an
object, and the printed MLIR name for a root. A name is used at one sort
only, and the tool refuses a second sort.

### Terms

A term is `{sort, [bits], op, args, ...}`. `sort` is `bv` (with `bits`),
`bool`, `ptr` (with `bits` = N) or `memory` (N-bit addresses, byte cells).
Numerals are decimal strings.

| op | args and fields |
|---|---|
| `bv` | `value` (unsigned, below 2^bits) |
| `bool` | `value` (true / false) |
| `var` | `name` |
| `ite`, `eq`, `ult`, `ule`, `not`, `and`, `or` | `args`; `and` and `or` are n-ary |
| `add`, `sub`, `mul`, `concat` | `args`; `add` and `mul` may be n-ary (left to right) |
| `extract` | one arg; `start`, `length` (bits) |
| `select`, `store` | memory, address[, value] |
| `constMem` | one arg: the value of every cell |
| `wselect`, `wstore` | memory, address[, value]; `bytes`, `alignment` (the promised alignment) |
| `mset`, `mcopy`, `mhavoc` | the memory-operation terms of `docs/design/SeaM-memops.md`, args as in Expr |
| `smap` | base memory, object; `entries` `[{offset, value}]`, newest first, numeral offsets |
| `addr`, `toInt`, `toBits`, `fromInt`, `fromBits` | one arg (`docs/design/PtrSort.md`) |
| `diff`, `ptrEq` | two pointers |
| `unitOverlap` | two pointers: the unit alias marker `IS_OVERLAP(p, 1, q, 1)` |
| `index` | one arg (the base); `indices` `[{stride, value}]`, `constant`, `byte_offset`, `flag` (`plain`, `nusw`, `nuw`) |
| `offset` | no arg; `indices`, `constant`, `byte_offset` |

### Firings

With `--firings` (a lane with rules), the tool records the rewriter's trail
(`include/sea/Expr/RewriteTrail.hh`, as `sea-vc --seam-trail` does). Each
rule call that changed its term gives one record, in order:

| key | value |
|---|---|
| `id` | the firing's number in the run |
| `rules` | the rule, then the decisions it rests on (`["row.store-skip", "cmp.diff-base"]`) |
| `premises` | the premise kinds used: `A-INB`, `LAYOUT`, `ALIGN`, `PTR-INDEX` |
| `op` | the VC operation: `load`, `store`, `addr` (an access address), or `side` / `query` (the final normalization) |
| `loc`, `units` | the operation's location and access size (when it has them) |
| `lhs`, `rhs` | raw terms. `lhs` is the term the rule was given, with its kids already rewritten |
| `premise` | a term: `SeaOpSem::firingPremise`, which is true, or the layout and the in-bounds conditions the rule relies on |

The statement is `premise -> lhs = rhs`, with pointers compared by address
and memories cell by cell. `sea-vc --seam-check` checks the same statement
with a solver after the boundary. A firing whose premise cannot be stated,
or whose terms the format cannot express, gets `unexpressible: <why>`
instead of `lhs`, `rhs` and `premise`, and a warning
`export.firing-unexpressible`. The status is then 3, and a witness check
must not count that firing. An unsatisfiable premise makes its firing
trivially valid. A checker should reject one, as `--seam-check` does.

## Fail closed

An operator, a sort or a width that the format has no node for, a program
that is not straight-line, an unknown lane, and an A-INB violation in an
A-INB lane are each an error (`[validation.export]`, status 1, nothing on
stdout). Nothing is skipped or approximated. Tests: `test/SeamExport/`.
