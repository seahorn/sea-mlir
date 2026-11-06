# Profiles

## The defaults are not tuned

The defaults of `sea-vc` are the conservative ones. `--lower` and `--seam`
are off, although every application on this site uses both. Whether a wide
access stays one term (`--seam-wide`), how far byte memories are widened
(`--widen-memory`), and where fat pointers and shadow memory are lowered
are open design choices; each needs measurements before it becomes a
default. Pass every option you rely on, or put it in a profile, and record
it with the result.

## The configurations in use

| workload | arguments |
|---|---|
| C applications (Firedancer, Stellar) | `--lower --seam --widen-memory=4` |
| aws-c-common proof harnesses (`vcc`) | `--lower --seam --widen-memory=8` |
| NKI kernels (traced, no C) | `--lower --seam` |
| bounded FD-FRAG jobs | add `--ptr-mode=address` (pointer and integer conversions) |

These are the configurations that the benchmark verdicts were established
with (the `defaults` of each family's `manifest.yaml`, plus per-job
`args`).

## Profiles

A profile is a file of options in LLVM config-file syntax, as for
`clang --config`. It turns a configuration into a name that can be
recorded and reused:

```text
# fd-reasm.cfg: the configuration of the reassembly proofs
--lower
--seam
--widen-memory=4
```

```console
$ sea-vc firedancer/step-mut-bounds-1.mlir --config=./fd-reasm.cfg --timeout=900 --mem-limit=16
tool.version: sea-mlir 0.1.0 (...)
vc.config: .../fd-reasm.cfg
...
seam.inb: assumed accesses=152 conditions=66
...
result: sat
```

- `--config` is repeatable. Later profiles override earlier ones, and the
  command line overrides every profile. List options accumulate.
- A bare name such as `--config=seam` is looked up as `seam.cfg` in the
  profile directories; a value with a `/` or an extension is a path.
- Each run prints `vc.config: <path>` for every profile it loaded, in
  order.

sea-mlir ships two example profiles, `seam` (`--lower --seam`) and
`seam-wide` (`seam` plus `--seam-wide`). They show the format; they are
not recommended defaults. Syntax, lookup and errors are on the
[Profiles reference](../reference/profiles.md).

## Options worth knowing

| option | when |
|---|---|
| `--solver=yices`, `--solver=bitwuzla` | z3 is the supported solver, but the fastest solver differs per workload. Try another before calling a job hard. |
| `--seam-stats` | rule counters; the first thing to compare between two configurations |
| `--vcgen-only` | query statistics without solving |
| `--seam-inb=0` | SeaM without the in-bounds assumption, for measurement. It is slower, and it is not a way to discharge the assumption. |
| `--seam-check` | checks every SeaM rewrite against its premises with the solver; an invalid rewrite gives exit status 5 |
| `--check-align`, `--check-ptr-index` | separate jobs that assert every trusted `align` or pointer-arithmetic promise of the front end |
| `--print-refs=strong` | keep every printed value alive through the lowering |
| `--ptr-mode=address` | admit integer-to-pointer casts |

Other modes (`--seam-wide`, `--seam-words`, `--mem=lambda`, and more) are
kept for measurement; `sea-vc --agent` lists them. When an alternative
wins on one workload, keep both as selectable modes and compare them by
their counters on the others.
