# Fuzzing

`sea-run --fuzz` runs a program many times on generated nondet values and
reports what the runs reached. Each trial is a `sea-run` run of the
program: it has the interpreter's semantics, the semantics `sea-vc`
encodes. A failing trial is a run that fails an assertion.

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
| `--keep-going` | count assertion failures instead of stopping at the first |
| `--input-file=<file>` | run once on a corpus file (without `--fuzz`) |
| `--max-steps=<n>` | the step limit of each trial |

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

## Failures and errors

A trial that fails a `sea.assert` is a failure. The fuzzer stops at the
first one, unless `--keep-going` is given. Its input is written as
`failure-<trial>.txt`.

A trial that stops with status `error` is recorded but does not stop the
fuzzer. Errors include accesses out of an object's bounds, through null or
to a freed object, and an access off a word of a word-layout memory
(`aligned`, `unaligned`). They also include runs the interpreter cannot
execute, such as a memory operation longer than its limit or `ptr.to_int`
in an abstract program. A broken `align` promise is not an error: the run
goes on, with the warning `interp.align-promise`. The first input that stops at
each operation is written as `error-<trial>.txt`.

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
fuzz.blocked: 30 op=sea.assume at=p.mlir:5:3
```

| line | meaning |
|---|---|
| `fuzz.failure: <runs> trial=<n> [file=<path>] at=<where>` | one per failing `sea.assert`: how many runs failed it, the first trial, its file |
| `fuzz.error: <runs> trial=<n> [file=<path>] op=<op> [site=<n>] at=<where>` | one per operation that stopped runs with an error; `site` is the operation's site, if it has one |
| `fuzz.error-message: <text>` | the message of the first such run |
| `fuzz.trials: <n> seeds=<n> time=<s> execs/s=<n>` | trials run (seed runs included), time, trials per second |
| `fuzz.status: halted=.. assert-failed=.. assume-blocked=.. step-limit=.. error=..` | runs per status |
| `fuzz.coverage: blocks=<n> blocks-total=<n> edges=<n> assumes=<n> assumes-total=<n>` | blocks entered, edges taken (the entry counts as one), `sea.assume` ops passed at least once |
| `fuzz.corpus: <entries> seeds=<files> skipped-lines=<n> files=<n> dictionary=<n>` | corpus entries at the end, seed files read, lines of saved runs that are not inputs, files written, dictionary size |
| `fuzz.blocked: <runs> op=<op> at=<where>` | one per `sea.assume` or `sea.unreachable` that blocked runs, most first |

`<where>` is a location as in the `assert-at:` line: the source position,
then `inlined-at=` and `name=` when there are any. With `--format=jsonl`,
each line is a `stat` record ([Output formats](output-formats.md)).

The `fuzz.blocked` lines show which assumptions stop the runs. A harness
whose runs are almost all blocked by one assume tests little beyond it:
that assume is the one to look at, or to seed past with a corpus entry.

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
