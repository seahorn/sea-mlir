# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Flags the query tools pass to a sea-vc binary only if it knows them.

The tools that read a written query as the VC (query-diff.py,
encoding-diff.py, cover-diff.py level 1, memops-equiv.py) turn the fold of
--vc-fold-top-eqs off: a folded query is equisatisfiable with the VC, not
equivalent. They may run an older binary (a base built from main) that does
not know the flag and does not fold, so the flag is passed only if the
binary lists it in its --help-hidden output (probed once per binary).
"""

import functools
import subprocess

NO_FOLD = "--vc-fold-top-eqs=false"


@functools.lru_cache(maxsize=None)
def knows(binary, name="vc-fold-top-eqs"):
    """Whether `binary --help-hidden` lists the option `name`."""
    try:
        p = subprocess.run(
            [binary, "--help-hidden"], capture_output=True, text=True, timeout=60
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return ("--" + name) in p.stdout


def no_fold(binary):
    """[--vc-fold-top-eqs=false] for a binary that has the fold, else []."""
    return [NO_FOLD] if knows(binary) else []
