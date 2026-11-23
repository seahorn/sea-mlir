# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# -*- Python -*-

import os
import shutil

import lit.formats
from lit.llvm import llvm_config

config.name = "SEA"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".mlir", ".test"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.sea_obj_root, "test")
config.excludes = ["Inputs", "CMakeLists.txt", "lit.cfg.py"]

# LLVM_PROFILE_FILE: a coverage build's profiles (utils/qa-coverage.py)
llvm_config.with_system_environment(["HOME", "TMP", "TEMP", "LLVM_PROFILE_FILE"])
# utils/qa-coverage.py's canon.fold events, on its run of test/QA only
# (--param sea_log=... --param sea_log_file=...); never the ambient SEA_LOG
for param, var in (("sea_log", "SEA_LOG"), ("sea_log_file", "SEA_LOG_FILE")):
    if lit_config.params.get(param):
        config.environment[var] = lit_config.params[param]
llvm_config.use_default_substitutions()

sea_tools_dir = os.path.join(config.sea_obj_root, "bin")
# the unittests/ drivers, kept out of bin/
sea_test_bin_dir = os.path.join(config.sea_obj_root, "test-bin")
llvm_config.with_environment("PATH", config.llvm_tools_dir, append_path=True)
llvm_config.add_tool_substitutions(
    [
        "sea-opt",
        "sea-reduce",
        "sea-run",
        "sea-vc",
        "sea-seam-export",
        "sea-expr-eval",
    ],
    [sea_tools_dir],
)
llvm_config.add_tool_substitutions(
    [
        "sea-expr-rewriter-test",
        "sea-seam-rules-test",
        "sea-bv-rules-test",
        "sea-addr-range-test",
        "sea-log-test",
        "sea-timer-test",
        "sea-stats-test",
        "sea-solver-factory-test",
        "sea-store-map-test",
        "sea-memops-test",
        "sea-ackermann-test",
        "sea-expr-core-test",
        "sea-expr-api-test",
        "sea-small-arith-test",
        "sea-join-cases-test",
        "sea-int-ranges-test",
        "sea-diff-decider-test",
        "sea-fold-top-eqs-test",
        "sea-canonical-sums-test",
    ],
    [sea_test_bin_dir],
)

config.substitutions.append(("%sea_src_root", config.sea_src_root))
config.substitutions.append(("%python", "python3"))
config.substitutions.append(("%sea_vc", os.path.join(sea_tools_dir, "sea-vc")))
config.substitutions.append(("%sea_bin", sea_tools_dir))

if config.z3 and not config.z3.endswith("NOTFOUND"):
    config.available_features.add("z3")
    config.substitutions.append(("%z3", config.z3))

# The Bitwuzla backend is optional (BITWUZLA_ROOT at configure time).
if config.bitwuzla == "ON":
    config.available_features.add("bitwuzla")

if config.yices == "ON":
    config.available_features.add("yices")

# The command-line solvers of those installs (reading a --smt2 query):
# yices-smt2 in YICES2_ROOT/bin, bitwuzla in BITWUZLA_ROOT/bin.
for feature, subst, path in (
    ("yices-bin", "%yices_smt2", config.yices_smt2),
    ("bitwuzla-bin", "%bitwuzla", config.bitwuzla_bin),
):
    if path and not path.endswith("NOTFOUND") and not path.startswith("@"):
        config.available_features.add(feature)
        config.substitutions.append((subst, path))

# Disabled tests carry `REQUIRES: disabled-test` and show up as UNSUPPORTED.
# `lit --param run_disabled=1` runs them anyway (to see if they pass again).
if lit_config.params.get("run_disabled"):
    config.available_features.add("disabled-test")

# The source tree is a git checkout (utils/spdx.py lists its files with git).
if os.path.exists(os.path.join(config.sea_src_root, ".git")) and shutil.which("git"):
    config.available_features.add("sea-git-tree")

# A test that this build cannot run (a solver it was built without, or the
# build without one: `UNSUPPORTED: yices`) is not collected at all, so a good
# run reports passes only. Only the build's optional parts count here;
# `REQUIRES: disabled-test` still shows as UNSUPPORTED, on purpose. lit
# excludes by file name, so a name used twice is left to lit.
BUILD_FEATURES = {
    "z3",
    "bitwuzla",
    "yices",
    "yices-bin",
    "bitwuzla-bin",
    "sea-git-tree",
}


def _not_for_this_build(path):
    with open(path, errors="replace") as f:
        for line in f:
            for key, wanted in (("REQUIRES:", True), ("UNSUPPORTED:", False)):
                _, sep, rest = line.partition(key)
                if not sep:
                    continue
                names = {n.strip() for n in rest.split(",")}
                if not names <= BUILD_FEATURES:
                    continue  # an expression, or not a build feature
                have = names <= config.available_features
                hit = names & config.available_features
                if (wanted and not have) or (not wanted and hit):
                    return True
    return False


def _excluded_for_this_build(root):
    seen, skip = {}, set()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in config.excludes]
        for name in filenames:
            if os.path.splitext(name)[1] not in config.suffixes:
                continue
            seen[name] = seen.get(name, 0) + 1
            if _not_for_this_build(os.path.join(dirpath, name)):
                skip.add(name)
    return sorted(n for n in skip if seen[n] == 1)


config.excludes += _excluded_for_this_build(config.test_source_root)
