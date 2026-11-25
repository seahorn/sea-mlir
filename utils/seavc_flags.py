# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Flags the query tools pass to a sea-vc binary only if it knows them.

The tools that read a written query as the VC (query-diff.py,
encoding-diff.py, cover-diff.py level 1, memops-equiv.py) turn the fold of
--vc-fold-top-eqs off: a folded query is equisatisfiable with the VC, not
equivalent. They may run an older binary (a base built from main) that does
not know the flag and does not fold, so the flag is passed only if the
binary lists it in its --help-hidden output (probed once per binary).

A probe that fails (the binary does not start, exits non-zero or times
out) is an error, not "an older binary": the tool cannot tell whether the
binary folds, and comparing folded queries would pass changes it must
report. The tools call require() before their first job and stop.
"""

import functools
import subprocess
import sys

NO_FOLD = "--vc-fold-top-eqs=false"
# seconds for one --help-hidden run
PROBE_TIMEOUT = 60


class ProbeError(RuntimeError):
    """`binary --help-hidden` failed: whether it folds is not known."""


@functools.cache
def knows(binary, name="vc-fold-top-eqs"):
    """Whether `binary --help-hidden` lists the option `name`; ProbeError if
    the probe fails."""
    try:
        p = subprocess.run(
            [binary, "--help-hidden"],
            capture_output=True,
            text=True,
            timeout=PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        raise ProbeError(
            f"cannot probe {binary} (--help-hidden): timed out after {PROBE_TIMEOUT} s"
        ) from None
    except (OSError, subprocess.SubprocessError) as e:
        raise ProbeError(f"cannot probe {binary} (--help-hidden): {e}") from None
    if p.returncode != 0:
        raise ProbeError(f"cannot probe {binary} (--help-hidden): exit {p.returncode}")
    return ("--" + name) in p.stdout


def no_fold(binary):
    """[--vc-fold-top-eqs=false] for a binary that has the fold, else []."""
    return [NO_FOLD] if knows(binary) else []


def require(binaries, code):
    """Probe each binary (cached for no_fold); on a failed probe print the
    error to stderr and exit with `code`, the caller's error status."""
    for b in binaries:
        try:
            knows(b)
        except ProbeError as e:
            print(
                f"error: seavc_flags: {e}; cannot tell whether it folds the "
                f"query (--vc-fold-top-eqs), so {NO_FOLD} cannot be decided",
                file=sys.stderr,
            )
            sys.exit(code)
