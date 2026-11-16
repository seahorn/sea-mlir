# SeaM export

`sea-seam-export` prints the SeaM terms of one straight-line
`sea.program` as one JSON document. It is the exporter for the conformance
checks of [seam-lean](https://github.com/agurfinkel/seam-lean), where the
pointer semantics of SeaM, its translation to bit-vectors, and its rewrite
rules are formalized in Lean. The formalization is not part of a sea-mlir
release; the checks run from seam-lean against a pinned sea-mlir revision.

The tool builds no term itself. The production code of `sea-vc` builds
every term: the encoding (`SeaOpSem`), the rewriter inside it
(`SeaMRewriter`), and the solver boundary (`SeaOpSem::solverTerm`). The
tool prints the terms as built and re-implements no rule. No solver runs.

## What the checks do

- **Semantics and translation.** For a concrete input, the value of every
  register in `sea-run` must equal Lean's evaluation of its raw term, of
  Lean's translation of that term to bit-vectors, and of the production
  translation (`erased_roots`).
- **Rule witnesses.** With `--firings`, every rule application of a run is
  a record `premise -> lhs = rhs`. A checker in Lean validates each one as
  an instance of a proved rule.

## Usage

```text
sea-seam-export <file.mlir> [--lane=<lane>] [--firings] [--address-bits=<N>]
```

The input is a module with exactly one `sea.program`. The program has one
block without arguments, ends in `sea.halt`, is placed (`sea.segment`,
`!sea.addr<N>`), and uses byte memories indexed by $N$-bit addresses
(default $N = 8$). It is taken as given, without lowering. `sea.umulo` and
`sea.smulo` are encoded with the native predicates (`umul_no_overflow` and
its siblings below), as `sea-vc` encodes them for its default solver z3
(and Bitwuzla); for Yices `sea-vc --lower` expands them first
(`sea-expand-intrinsics`); without `--lower` it refuses them
(`vc.intrinsic`). The pointer
mode is `address` if the program has `sea.addr.from_int`, and `opaque`
otherwise.

```console
$ sea-seam-export smap.mlir --lane=seam --firings | jq -c '{schema, lane, sea_vc, ainb, rewrite, roots: (.roots|length), firings: (.firings|length)}'
{"schema":3,"lane":"seam","sea_vc":"sea-vc --seam","ainb":true,"rewrite":true,"roots":13,"firings":4}
$ sea-seam-export smap.mlir --lane=seam --firings | jq -c '.firings[0] | {id, rules, premises, op}'
{"id":0,"rules":["smap.new"],"premises":[],"op":"store"}
```

(`smap.mlir` is the regression test `test/SeamExport/lanes-store-map.mlir`.)

Exit status: 0 exported; 1 error, with nothing on stdout; 3 exported, but
some firings cannot be expressed.

## Lanes

A lane is one configuration of the encoding. The `sea_vc` key of the
output names the `sea-vc` command whose query terms the lane builds; it is
empty when no `sea-vc` run builds them.

| lane | `sea_vc` | what |
|---|---|---|
| `memory-rules0` (default) | | the `--seam --seam-wide` encoding with the memory rules, simplification, the layout decision and the premises off; the pointer rules run at accesses |
| `seam-wide-inb0-simplify0-layout0` (`--rewrite`) | `sea-vc --seam --seam-wide --seam-premises=0 --seam-simplify=0 --seam-layout=0` | memory rules on |
| `raw-wide` | | the `--seam --seam-wide` encoding before any rule |
| `raw` | | the `--seam` encoding (byte accesses) before any rule |
| `seam-wide-inb0` | `sea-vc --seam --seam-wide --seam-premises=0` | |
| `seam-inb0` | `sea-vc --seam --seam-premises=0` | |
| `seam-wide` | `sea-vc --seam --seam-wide` | under the premises (`"ainb": true`) |
| `seam` | `sea-vc --seam` | under the premises (`"ainb": true`) |
| `seam-stride` | `sea-vc --seam --seam-stride` | under the premises (`"ainb": true`); R-STRIDE can fire |
| `seam-inb0-stride` | `sea-vc --seam --seam-premises=0 --seam-stride` | byte accesses, no premises; R-STRIDE can fire |
| `seam-wide-stride` | `sea-vc --seam --seam-wide --seam-stride` | wide accesses, under the premises (`"ainb": true`); R-STRIDE can fire |
| `plain` | `sea-vc` | no SeaM and no rewriter |
| `simplify` | `sea-vc --simplify` | no SeaM; the bit-vector and Boolean rules only |

- Lanes without wide accesses (`raw`, `seam-inb0`, `seam`, `seam-stride`, `seam-inb0-stride`, `plain`,
  `simplify`) refuse a multi-byte access; `sea-vc --lower` would split it.
- A premise lane (`seam-wide`, `seam`, `seam-stride`, `seam-wide-stride`) is an error when the program has a static out-of-object
  access (its firings would rest on a premise the program may break;
  `sea-vc` reports it and keeps the premises).
- In `plain` and `simplify`, an object is its placed address, not a
  variable `obj.<name>`.

## Format (schema 3)

One JSON object with sorted keys:

| key | value |
|---|---|
| `schema` | `3` (2 before the premises were renamed: `premises` values, `sea_vc` flags). Additions keep the number; an incompatible change increments it |
| `lane`, `sea_vc` | the lane and its `sea-vc` command |
| `ptr_mode` | `address` or `opaque` |
| `address_bits` | $N$ |
| `ainb` | whether the premises A-SEP, A-NULL, A-NOWRAP were used (the key keeps its name) |
| `rewrite` | whether the memory rules ran |
| `roots` | `[{name, location, operation, term}]`: every integer, address and memory result, in program order |
| `side` | `[term]`: the side conditions, such as the layout `addr(obj.<name>) = base` |
| `sources` | `[{site, term}]` for nondet values, nondet memories and havoc arrays, and `[{fixed, term}]` for segments with their placed base |
| `probes` | `[{location, bytes, memory, address}]`: the operands of each load |
| `erased_roots`, `erased_side` | the non-memory roots and the side conditions after the solver boundary, with no pointer sort left |
| `firings`, `firings_unexpressible` | with `--firings`: the rule applications, and how many could not be expressed |

Names are the query's: `nd.<site>_0` for an input, `obj.<name>` for an
object, and the printed MLIR name for a root.

### Terms

A term is `{sort, [bits], op, args, ...}`. `sort` is `bv` (with `bits`),
`bool`, `ptr` (with `bits` = $N$) or `memory` ($N$-bit addresses, byte
cells). Numerals (the `bv` value, `smap` offsets, `fixed`) are strings in
the form of the Expr printer: decimal below 65535, lowercase hex with a
`0x` prefix from 65535 up (`0x3ffff80`). A `bv` value is unsigned; a
negative one, which should not occur, would print as `-5` or `0x-ffff`.
The `stride`, `constant` and `byte_offset` fields of `index` and `offset`
are signed decimal.

| op | arguments and fields |
|---|---|
| `bv` | `value` (unsigned, below $2^{bits}$) |
| `bool` | `value` |
| `var` | `name` |
| `ite`, `eq`, `ult`, `ule`, `not`, `and`, `or` | `args`; `and` and `or` are n-ary |
| `add`, `sub`, `mul`, `concat` | `args`; `add` and `mul` may be n-ary; `concat` may be n-ary, first arg most significant |
| `extract` | one argument; `start`, `length` (bits) |
| `slt`, `sle` | two bit-vector args of one width; a `bool` term: signed (two's complement) less-than, less-or-equal |
| `ne` | the arguments of `eq`; a `bool` term: the negation of `eq` |
| `ugt`, `uge`, `sgt`, `sge` | two bit-vector args of one width; a `bool` term: unsigned / signed (two's complement) greater-than, greater-or-equal |
| `udiv`, `sdiv`, `urem`, `srem` | two bit-vector args of one width; a bit-vector of that width. SMT-LIB total: x `udiv` 0 is all ones, x `urem` 0 is x; `sdiv` and `srem` are the signed forms by the sign cases of SMT-LIB `bvsdiv` / `bvsrem` (x `sdiv` 0 is -1 for x >= 0 and 1 for x < 0, x `srem` 0 is x; the `srem` result has the sign of x (when nonzero); INT_MIN `sdiv` -1 is INT_MIN and INT_MIN `srem` -1 is 0, the wrapped results) |
| `bvand`, `bvor`, `bvxor` | bit-vector args of one width, at least two, in order (n-ary is allowed, as `add`); a bit-vector of that width |
| `shl`, `lshr`, `ashr` | two bit-vector args of one width: the value and the shift amount (unsigned, same width); a bit-vector of that width. A shift amount of at least the width gives 0 (`shl`, `lshr`) or the sign fill, all ones for a negative value and 0 otherwise (`ashr`; below the width it fills with the sign bit) |
| `sext`, `zext` | one bit-vector arg; the result's `bits` (at least the arg's `bits`): the arg sign- or zero-extended |
| `umul_no_overflow`, `smul_no_overflow`, `smul_no_underflow` | two bit-vector args of one width $n$; a `bool` term over the product of the args as unbounded integers: `umul_no_overflow` is true iff the unsigned product is below $2^n$; `smul_no_overflow` iff the signed (two's complement) product is at most $2^{n-1} - 1$; `smul_no_underflow` iff it is at least $-2^{n-1}$. z3's `Z3_mk_bvmul_no_overflow` (unsigned / signed) and `Z3_mk_bvmul_no_underflow`. `sea.umulo` is `not(umul_no_overflow(a, b))` and `sea.smulo` is `not(and(smul_no_overflow(a, b), smul_no_underflow(a, b)))` (`bv::mulOverflows`), the overflow bits sea-run computes (`APInt::umul_ov` / `smul_ov`: the product does not fit in $n$ bits) |
| `select`, `store` | memory, address[, value] |
| `constMem` | the value of every cell |
| `wselect`, `wstore` | memory, address[, value]; `bytes`, `alignment` |
| `mset`, `mcopy`, `mhavoc` | the memory-operation terms |
| `smap` | base memory, object; `entries` `[{offset, value}]`, newest first |
| `addr`, `toInt`, `toBits`, `fromInt`, `fromBits` | one argument |
| `diff`, `ptrEq` | two pointers |
| `unitOverlap` | two pointers: the alias marker of two one-byte accesses |
| `index` | the base; `indices` `[{stride, value}]`, `constant`, `byte_offset`, `flag` (`plain`, `nusw`, `nuw`) |
| `offset` | `indices`, `constant`, `byte_offset` |

### Firings

With `--firings`, each rule call that changed its term gives one record,
in order:

| key | value |
|---|---|
| `id` | the firing's number in the run |
| `rules` | the rule, then the decisions it rests on |
| `premises` | the premise kinds used: `A-SEP`, `A-NULL`, `A-NOWRAP`, `LAYOUT`, `ALIGN`, `PTR-INDEX` |
| `op` | `load`, `store`, `addr`, or `side` / `query` for the final normalization |
| `loc`, `units` | the location and access size, when present |
| `lhs`, `rhs` | raw terms; `lhs` is the term the rule was given |
| `premise` | a term: true, or the layout and in-bounds conditions the rule relies on; a raw term like `lhs` and `rhs` (with `PTR-INDEX`: the `sea.ptr.index` promise, `sle` over `sext` / `zext` at a wider width; with `ALIGN`: `eq(bvand(addr(p), al - 1), 0)` per promised alignment `al`) |

The statement is `premise -> lhs = rhs`, with pointers compared by address
and memories cell by cell. `sea-vc --seam-check` checks the same statement
with a solver. A firing whose premise or terms the format cannot express
carries `unexpressible: <why>` instead, with the warning
`export.firing-unexpressible`, and the exit status is 3. A checker must not
count such a firing, and should reject a firing whose premise is
unsatisfiable.

## Fail closed

An operator, sort or width that the format has no node for, a program
that is not straight-line, an unknown lane, and a static out-of-object access in a
premise lane are each an error (`[validation.export]`, exit status 1,
nothing on stdout). Nothing is skipped or approximated.
A program the VC cannot encode is an error of the VC's kind, with the
location, status 1 and nothing on stdout, for example: an abstract (unplaced) program
(`[vc.abstract-program]`, as `sea-vc` refuses it), an op `SeaOpSem` does
not encode (`[vc.unsupported-op]`), an intrinsic it leaves to
`sea-expand-intrinsics` (`[vc.intrinsic]`), a multi-byte access in a lane
without wide accesses (`[vc.wide-access]`).
