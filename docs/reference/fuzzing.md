# Fuzzing

`sea-run --fuzz` runs a program many times on generated nondet values and
reports what the runs reached. Each trial is a `sea-run` run of the
program: it has the interpreter's semantics, the semantics `sea-vc`
encodes. A failing trial is a run that fails an assertion, or, with
`--defer-memory-errors`, a run with a memory-safety violation that no
assumption blocks.

A fuzzing run without failures says nothing about the inputs it did not
try; `sea-vc` checks all of them.

## Running it

```console
$ sea-run p.mlir --fuzz=10000 --seed=1 --fuzz-out=out
```

| option | effect |
|---|---|
| `--fuzz=<n>` | run `n` trials |
| `--fuzz-time=<s>` | run for at most `s` seconds (with `--fuzz`: whichever ends first) |
| `--seed=<n>` | the seed of every random choice |
| `--corpus=<dir or file>,...` | seed entries: corpus files, or directories of them (their files in name order) |
| `--fuzz-out=<dir>` | write failing inputs and inputs that reached new coverage as corpus files; the directory must be new or empty |
| `--fuzz-dict=false` | no dictionary values (see below) |
| `--keep-going` | count failures instead of stopping at the first |
| `--defer-memory-errors` | go on past memory-safety violations; one is a failure only if the run reaches the end (see below) |
| `--input-file=<file>` | run once on a corpus file (without `--fuzz`) |
| `--max-steps=<n>` | the step limit of each trial |
| `--fuzz-sites` | list the input sites and exit, without running (see [For agents](#for-agents)) |

All trials run in one process: the program is parsed once.

## Trials

The seeds run first, in order. Every later trial picks an entry of the
corpus, or the empty entry, and changes one, two, four or eight of its
values. A change is one of:

- a fresh random value;
- one bit flipped;
- plus or minus one;
- 0, 1, -1, or the signed minimum or maximum of the width;
- a dictionary value.

A site or memory cell that the entry does not set gets a random value or a
dictionary value. Every value fits the width of its site.

The dictionary holds the integer constants that the program compares with
or computes with (`sea.icmp`, add, sub, mul, div, rem, min, max and the
overflow tests), each with its neighbors plus and minus one. A value is
sign- or zero-extended or truncated to the site's width. It gets runs past
guards such as `assume(x == 1234)` that random values miss.

A site has one value per trial, as with `--input`: a site that a loop reads
again reads the same value. So every trial replays through `--input-file`.

## Coverage

A trial reaches new coverage when it enters a block it has not entered
before, takes a new edge between blocks, or passes a `sea.assume` that no
earlier run passed. Its input then joins the corpus, and with `--fuzz-out`
it is written as `cov-<trial>.txt`.

## Failures, memory-safety checks and limits

A trial that fails a `sea.assert` is a failure. With
`--defer-memory-errors`, so is a memory-safety finding (status
`memory-error`, below). The fuzzer stops at the first failure, unless
`--keep-going` is given. Its input is written as `failure-<trial>.txt`.

A trial that stops with status `error` is recorded per operation but does
not stop the fuzzer. There are two kinds:

- A memory-safety check: an access through null, out of an object's
  bounds or to a freed object, a double free, or a free of a pointer that
  is not the start of a heap object. Without `--defer-memory-errors` the
  run stops there, so whether the run was feasible is unknown: an assume
  later on the path might block it. These are `fuzz.memory-check` lines,
  and the first input of each is written as `memory-check-<trial>.txt`.
- A limit: a run the interpreter cannot execute, such as a memory
  operation longer than its limit, `ptr.to_int` in an abstract program,
  a pointer loaded from bytes that do not hold one, or an unsupported
  operation. An access off a word of a word-layout memory (`aligned`,
  `unaligned`) counts here too. Limits are never findings and are never
  deferred. These are `fuzz.limit` lines, with the warning `fuzz.limit`
  (once, then counted), and the first input of each is written as
  `limit-<trial>.txt`.

A broken `align` promise is not an error: the run goes on, with the
warning `interp.align-promise`.

## Deferred memory errors

A run can reach a memory-safety violation and only later reach an
assumption that blocks it. Such a run is infeasible, and the violation is
not a bug. This happens with assumptions placed late: unified assumes
(one assume of all conditions, just before the assertion), prophecy
variables, and programs from other front ends. With
`--defer-memory-errors` (a `sea-run` option, also without `--fuzz`), the
interpreter does not stop at a violation. It records the first one, counts
them all, and goes on:

- a load reads 0, or the null pointer for a pointer load;
- a store, `memset`, `memmove` or `memhavoc` with a violation does
  nothing (none of its cells change);
- a bad `free` does nothing.

The classification assumes that past the first violation any behaviour
is allowed: the access leaves every object, and the model does not say
what happens next. The substitutes pick one continuation, and only decide
whether a later assumption holds. A finding means that the violation is
reached on an exact run of the model up to it, and that some continuation
reaches the end. Under this assumption, a run that a later assume blocks
with the substitutes might pass it with other values, so deferring can miss
a finding; it does not make one up. The substitutes are fixed, so a run
replays.

A finding need not be a counterexample of a VC that gives memory array
semantics past the violation. An out-of-bounds store is dropped here, while
`sea-vc`'s arrays read the stored value back: a program that stores 7 out
of bounds, loads it back, assumes the value is 0 and halts is a finding
here and infeasible there. What happens past a violation is left to the
formal semantics of sea-mlir.

At the end of the run:

| the run | status |
|---|---|
| is blocked by an assume | `assume-blocked`, with `deferred: <n>`: infeasible, not a finding |
| reaches `sea.halt` or a failing `sea.assert` | `memory-error`: a finding, at the first violation, with `reached:` |
| hits the step limit or a limit | its status, with `deferred: <n>`: not classified |

A run that fails an assertion after a violation is reported as the
violation. The assertion's condition may depend on the substitutes, and
the violation came first; once it is fixed, a new run shows the assertion.

```text
status: memory-error
at: loc("p.mlir":8:8)
reached: halted at=p.mlir:11:3
message: memory access out of bounds: offset 9 in stack.1 of size 4
deferred: 1 kind=out-of-bounds at=p.mlir:8:8
```

`at:` is the violation. `reached:` is the status and the location of the
operation that made it reachable. `deferred:` gives the number of
violations in the run and the kind (`null`, `out-of-bounds`,
`use-after-free`, `double-free`, `bad-free`) and location of the first.
`sea-run` exits with 4 on `memory-error`, like `sea-vc`'s 4 (the in-bounds
assumption A-INB does not hold), plus double free and bad free.

Under `--fuzz`, a memory-safety finding is a failure: its file is
`failure-<trial>.txt`, it is replayed and printed as a plain run, and the
fuzzer stops at it unless `--keep-going` is given. It is a
`fuzz.memory-safety` line, and the fuzzer exits with 4. Replay its file
with `--defer-memory-errors`.

Programs whose assumptions are not unified (each assume at its own site)
need deferral less: there, an assume blocks before the accesses after it.

## Output

When there is a failure, the output starts with the first one, as a plain
run on its input: the lines that `sea-run --input-file=<its file>` prints.
The fuzzer replays it before printing and checks that it ends in the same
status at the same operation (`fuzz.replay-mismatch` if not).

Then come the `fuzz.*` lines:

```text
fuzz.failure: 1 trial=32 file=out/failure-32.txt at=p.mlir:12:3
fuzz.trials: 32 seeds=0 time=0.001 execs/s=54659
fuzz.status: halted=1 assert-failed=1 assume-blocked=30 step-limit=0 error=0
fuzz.coverage: blocks=3 blocks-total=3 edges=3 assumes=1 assumes-total=1
fuzz.corpus: 3 seeds=0 skipped-lines=0 files=3 dictionary=6
fuzz.blocked: 30 op=sea.assume at=p.mlir:5:3 sites=0 cond=(eq s0 1234)
```

| line | meaning |
|---|---|
| `fuzz.failure: <runs> trial=<n> [file=<path>] at=<where>` | one per failing `sea.assert`: how many runs failed it, the first trial, its file |
| `fuzz.memory-safety: <runs> trial=<n> [file=<path>] op=<op> [site=<n>] at=<where>` | `--defer-memory-errors`: one per operation with a memory-safety finding (the first violation of a run that reached the end or a failing assert) |
| `fuzz.memory-safety-message: <text>` | the message of the first such run |
| `fuzz.memory-check: <runs> trial=<n> [file=<path>] op=<op> [site=<n>] at=<where>` | without `--defer-memory-errors`: one per operation that stopped runs at a memory-safety check; `site` is the operation's site, if it has one |
| `fuzz.memory-check-message: <text>` | the message of the first such run |
| `fuzz.limit: <runs> trial=<n> [file=<path>] op=<op> [site=<n>] at=<where>` | one per operation that stopped runs at an interpreter limit or a layout error |
| `fuzz.limit-message: <text>` | the message of the first such run |
| `fuzz.trials: <n> seeds=<n> time=<s> execs/s=<n>` | trials run (seed runs included), time, trials per second |
| `fuzz.status: halted=.. assert-failed=.. assume-blocked=.. step-limit=.. error=.. [memory-error=..]` | runs per status (`memory-error` with `--defer-memory-errors`) |
| `fuzz.deferred: <runs> memory-error=.. assume-blocked=.. step-limit=.. error=..` | `--defer-memory-errors`: the runs that went past a violation, by how they ended |
| `fuzz.coverage: blocks=<n> blocks-total=<n> edges=<n> assumes=<n> assumes-total=<n>` | blocks entered, edges taken (the entry counts as one), `sea.assume` ops passed at least once |
| `fuzz.corpus: <entries> seeds=<files> skipped-lines=<n> files=<n> dictionary=<n>` | corpus entries at the end, seed files read, lines of saved runs that are not inputs, files written, dictionary size |
| `fuzz.blocked: <runs> op=<op> at=<where> [sites=<n,...> cond=<expr>]` | one per `sea.assume` or `sea.unreachable` that blocked runs, most first; for a `sea.assume`, its condition over the input sites (see [For agents](#for-agents)) |

`<where>` is a location as in the `assert-at:` line: the source position,
then `inlined-at=` and `name=` when there are any. With `--format=jsonl`,
each line is a `stat` record ([Output formats](output-formats.md)).

The `fuzz.blocked` lines show which assumptions stop the runs. A harness
whose runs are almost all blocked by one assume tests little beyond it:
that assume is the one to look at, or to seed past with a corpus entry.

## For agents

Two outputs let a program outside `sea-mlir` (an agent's script, say) see
why runs are blocked and choose inputs to get past them, without reading
the IR.

`--fuzz-sites` lists the input sites of the program, in program order,
and exits without running it:

```console
$ sea-run placed.mlir --fuzz-sites
fuzz.site: 0 callee=region.0 width=8 input=memory op=sea.mem.nondet at=placed.mlir:50:11 name=sm15
...
fuzz.site: 6 callee=nd_size_t width=64 input=value op=sea.nondet at=seahorn/lib/byte_buf_helper.c:19:18 name=_4
fuzz.site: 7 callee=nd_size_t width=64 input=value op=sea.nondet at=seahorn/lib/byte_buf_helper.c:20:18 name=_5
fuzz.site: 8 callee=nd_malloc_is_fail width=1 input=value op=sea.nondet at=seahorn/lib/sea_allocators.c:11:10 name=_13
...
fuzz.sites: 17 value=7 memory=10
```

(the vcc job `byte_buf_append`, lowered as in
[Programs that do not run](#programs-that-do-not-run)).


| field | meaning |
|---|---|
| value | the site number: the key of `--input` and corpus assignments |
| `callee` | the nondet function (`sea.nondet`, `sea.struct.nondet`), the memory's name (`sea.mem.nondet`), or `-` (`sea.memhavoc`) |
| `width` | the value's width in bits; for a memory, the width of a cell |
| `input` | `value`: one value per run (`<site>=<v>`); `memory`: one value per cell read (`<site>@<addr>=<v>`) |
| `op` | the operation |
| `at` | its location, as in `<where>` (with `inlined-at=` and `name=` when there are any) |

The last line, `fuzz.sites: <n> value=<n> memory=<n>`, counts them; `0`
means the program reads no input. `sea.ptr.nondet` is not listed: it
makes a fresh object and takes no input. `--fuzz-sites` refuses the
options that would have no effect (`--fuzz`, `--fuzz-time`, `--input`,
`--input-file`, `--trace`, `--seed`, `--max-steps`,
`--defer-memory-errors`).

Each `fuzz.blocked` line of a `sea.assume` ends with the assume's
condition written over the input sites:

```text
fuzz.blocked: 3054 op=sea.assume at=seahorn/lib/byte_buf_helper.c:21:5 sites=6,7 cond=(ule s6 s7)
fuzz.blocked: 2837 op=sea.assume at=seahorn/lib/byte_buf_helper.c:22:5 sites=7 cond=(ult s7 11)
fuzz.blocked: 922 op=sea.assume at=seahorn/jobs/byte_buf_append/aws_byte_buf_append_harness.c:13:5 sites=- cond=?
```

Read it as: the assume holds when site 6 is at most site 7 (unsigned),
and when site 7 is below 11. Corpus entries with `7` in 0..10 and `6` at
most `7` pass both. The third assume's condition is a value merged from
several paths, so the walk stops at once.

`cond` is an s-expression:

- `s<N>`: the value of input site N;
- a number: a constant, in unsigned decimal (so -1 in 8 bits is 255);
  a placed object's base address is a constant too;
- `(<pred> a b)`: a comparison, with the predicate of `sea.icmp` (`eq`,
  `ne`, `ult`, `ule`, `ugt`, `uge`, `slt`, `sle`, `sgt`, `sge`); it
  compares at the width of its operands;
- `(<op> a ...)`: an operation whose value the text fully shows, named
  as in the IR without `sea.`: the arithmetic and bitwise operations,
  min and max, the overflow tests, `concat`, `select`, `struct.create`,
  the `addr.*` operations, and `ptr.add`, `ptr.diff`, `ptr.null`. The
  `nsw` and `nuw` promises show as `.nsw` and `.nuw` (`(add.nsw a b)`).
  Operations that change the width carry the result width:
  `(zext:64 a)`, `(trunc:8 a)`, `(extract:8 a <low>)`;
- `(struct.extract s <index>)`, `(struct.insert s v <index>)`,
  `(global @<name>)`, `(func_addr @<name>)`, and `(ptr.index <base>
  <indices> strides=<s,...> offset=<n>)` with `inbounds`, `nusw`, `nuw`
  when set;
- `(load:<w> s<N>@<addr>)`: a `w`-bit load from the memory of site N, a
  `sea.mem.nondet` whose only users are loads, so the cells are still
  the inputs; it reads the cells at `addr` and after, low byte first;
- `?`: where the walk stopped.

`sites` lists the sites that `cond` names, or `-` for none. `cond` is the
last field and runs to the end of the line, because it holds spaces; in
`--format=jsonl`, `sites` and `cond` are string fields.

The condition is computed once per blocking assume, from the program text,
when the report prints; the trials do not pay for it. It is a backward walk
from the assume's operand, not a symbolic execution. It stops with `?` at:

- any operation not listed above: other memory reads (a load from a
  memory that an update uses, even in place), operations with effects
  such as allocations, and operations whose value depends on more than
  their operands (`ptr.base`, `ptr.dereferenceable`, `ptr.to_int`);
  also a listed operation with an attribute the text does not show;
- a block argument with more than one incoming value (a value merged
  from several paths, such as the result of `a && b` in C); a block
  argument with a single incoming value is followed;
- an operation more than 6 levels below the condition (sites and
  constants are shown at any depth);
- any node past the 40th.

The condition says nothing about the path to the assume: a run that does
not reach it is not blocked by it. `cond=0` is an assume of a constant
false: no input passes it, so the code after it is unreachable.

## Corpus files

A corpus file holds one input: one assignment per line.

```text
# a comment
0=1234
2@16=7
3@heap.4+8=0xff
```

An assignment is in the `--input` syntax: `<site>=<value>` for a nondet
site, `<site>@<addr>=<value>` for a memory cell, `<site>@<object>+<offset>=<value>`
for a cell of an abstract memory. The `nondet:` and `memread:` lines of a
`sea-run` or `sea-vc` output are read too, and the other `key: value` lines
of such an output are skipped, so a saved run is a seed:

```console
$ sea-run p.mlir --input=0=1234 > seed.txt
$ sea-run p.mlir --fuzz=10000 --corpus=seed.txt
```

Blank lines and `#` comments are skipped. Any other line is an error, as is
a value that does not fit its site.

## Reproducibility

The same program, `--seed`, corpus and options give the same trials and the
same output, except the `time` and `execs/s` fields. With `--fuzz-time`, the
number of trials depends on the machine's speed, so the result is
reproducible only up to where the time ran out: give `--fuzz` for a fixed
run.

## Logs

`--log=fuzz.run` shows the start and end of the run; `--log=fuzz.run:2`
also shows each trial that reached new coverage ([Logging](logging.md)).

## Programs that do not run

The fuzzer runs what `sea-run` runs. An abstract program (pointers into
unplaced objects) runs directly, but stops on operations that need
addresses, such as `ptr.to_int`. Such a program can be fuzzed after the
lowering `sea-vc --lower` runs, which places the objects:

```console
$ sea-opt p.mlir --mlir-print-debuginfo --sea-apply-tracking \
    --sea-scalarize-structs --sea-drop-fresh-memhavoc --sea-lower-tracking \
    --sea-lower-fat-pointers --sea-place-static --sea-vc-lower -o placed.mlir
$ sea-run placed.mlir --fuzz=10000
```
