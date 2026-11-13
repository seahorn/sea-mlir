#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""A stand-in for sea-vc in bench.py tests: the job file is not a program but a
script of what to print and how to end. Lines `out: <text>` / `err: <text>`
print to stdout / stderr, `rc: <n>` exits with n, `signal: <NAME>` kills the
process with SIGNAME. Other options (--help-hidden, sea-vc args) are ignored."""

import os
import signal
import sys

job = next((a for a in sys.argv[1:] if not a.startswith("-")), None)
if job is None:
    sys.exit(0)
rc, sig = 0, None
for line in open(job):
    key, _, val = line.rstrip("\n").partition(": ")
    if key == "out":
        sys.stdout.write(val + "\n")
    elif key == "err":
        sys.stderr.write(val + "\n")
    elif key == "rc":
        rc = int(val)
    elif key == "signal":
        sig = getattr(signal, "SIG" + val)
sys.stdout.flush()
sys.stderr.flush()
if sig is not None:
    import resource

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))  # no core file
    signal.signal(sig, signal.SIG_DFL)
    os.kill(os.getpid(), sig)
sys.exit(rc)
