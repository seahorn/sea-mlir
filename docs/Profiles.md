# sea-vc profiles (`--config`)

A profile is a named set of `sea-vc` options in a file. `sea-vc` reads it
with LLVM's config-file machinery (`llvm::cl::ExpansionContext`, config
mode: the same as `clang --config`).

```bash
sea-vc p.mlir --config=seam                  # profiles/seam.cfg
sea-vc p.mlir --config=./my.cfg --timeout=60 # a file; the command line adds
sea-vc p.mlir --config=seam --seam=false     # the command line overrides
```

## Syntax

- options separated by whitespace or newlines, as on a command line;
- `#` starts a comment (to the end of the line);
- `@file` includes another file; relative paths resolve against the
  directory of the including file; includes nest; `<CFGDIR>` expands to
  that directory.

## Semantics

`--config` is repeatable. The options of the configs are inserted, in the
order given, before the options of the command line. LLVM keeps the last
value of an option, so a later config overrides an earlier one, and the
command line overrides every config. List options (`--z3-param`, `--log`)
accumulate.

`sea-vc` prints `vc.config: <resolved path>` for each config it loads, in
order, before anything else (stdout; stderr with `--smt2`).

## Lookup

A value with a `/` or an extension is a path (relative to the current
directory). A bare name `<name>` is the file `<name>.cfg`, looked up in:

1. the directories of `$SEA_MLIR_PROFILE_PATH` (colon-separated, in order);
2. `<prefix>/share/sea-mlir/profiles` of an install
   (`../share/sea-mlir/profiles` from the `sea-vc` binary);
3. the source tree's `profiles/` (compiled in as `SEA_SOURCE_PROFILES_DIR`:
   build trees find the shipped profiles without an install).

The first match wins. Not found: error `vc.config-not-found`, listing the
directories searched; a file that cannot be read, or a bad include:
`vc.config-read`. Both exit with status 1.

Inside a config, include another with `@file`. A `--config` inside a config
is handled by LLVM itself (a path, or a file name with its extension in the
directories above) and is not printed as `vc.config`.

## Shipped profiles

`profiles/` is installed to `share/sea-mlir/profiles`. Its files are
examples of the format, not recommended defaults (the defaults of `sea-vc`
are still open):

- `seam.cfg`: `--lower --seam`;
- `seam-wide.cfg`: `@seam.cfg` plus `--seam-wide`.
