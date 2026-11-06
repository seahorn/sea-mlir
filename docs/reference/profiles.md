# Profiles

A profile is a named set of `sea-vc` options in a file. `sea-vc` reads it
with LLVM's config-file support, the same mechanism as `clang --config`.

```bash
sea-vc p.mlir --config=seam                   # the shipped profiles/seam.cfg
sea-vc p.mlir --config=./my.cfg --timeout=60  # a file; the command line adds options
sea-vc p.mlir --config=seam --seam=false      # the command line overrides the profile
```

## Syntax

- Options are separated by whitespace or newlines, as on a command line.
- `#` starts a comment that runs to the end of the line.
- `@file` includes another file. A relative path is resolved against the
  directory of the including file, includes nest, and `<CFGDIR>` expands
  to that directory.

## Semantics

`--config` is repeatable. The options of the profiles are inserted, in the
order given, before the options of the command line. The last value of an
option counts, so a later profile overrides an earlier one and the command
line overrides every profile. List options, such as `--z3-param` and
`--log`, accumulate.

`sea-vc` prints `vc.config: <resolved path>` for each profile it loads, in
order (on stderr with `--smt2`).

## Lookup

A value with a `/` or an extension is a path, relative to the current
directory. A bare name `<name>` is the file `<name>.cfg`, looked up in:

1. the directories of `$SEA_MLIR_PROFILE_PATH`, separated by colons, in
   order;
2. `<prefix>/share/sea-mlir/profiles` of an install, found relative to the
   `sea-vc` binary;
3. the source tree's `profiles/`, so that a build tree finds the shipped
   profiles without an install.

The first match wins. A profile that is not found is the error
`vc.config-not-found`, which lists the directories searched. A file that
cannot be read, or a bad include, is the error `vc.config-read`. Both exit
with status 1.

A `--config` inside a profile is handled by LLVM itself and is not printed
as `vc.config`; include other files with `@file` instead.

## Shipped profiles

`profiles/` is installed to `share/sea-mlir/profiles`. Its files are
examples of the format, not recommended defaults:

- `seam.cfg`: `--lower --seam`;
- `seam-wide.cfg`: `@seam.cfg` and `--seam-wide`.

```text
# seam-wide.cfg - an EXAMPLE sea-vc profile (sea-vc --config=seam-wide), not
# a recommended default: the defaults of sea-vc are still open.
#
# seam.cfg (included; @-paths are relative to this file), plus --seam-wide:
# wide (multi-byte) accesses stay whole under SeaM instead of being split
# into bytes by the lowering.
@seam.cfg
--seam-wide
```
