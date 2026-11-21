# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Reads the `check-inb-dir: <path>` line of a sea-vc --check-inb output and
# says whether that directory exists and holds manifest.json; then removes
# it (a test leaves no temporary directory behind).
import os
import shutil
import sys

with open(sys.argv[1]) as f:
    for line in f:
        if line.startswith("check-inb-dir: "):
            d = line[len("check-inb-dir: ") :].strip()
            print("dir-exists" if os.path.isdir(d) else "dir-missing")
            print(
                "manifest-exists"
                if os.path.isfile(os.path.join(d, "manifest.json"))
                else "manifest-missing"
            )
            shutil.rmtree(d, ignore_errors=True)
