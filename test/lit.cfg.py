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
llvm_config.with_environment("PATH", config.llvm_tools_dir, append_path=True)
llvm_config.add_tool_substitutions(
    [
        "sea-opt",
        "sea-reduce",
        "sea-run",
        "sea-expr-eval",
        "sea-vc",
        "sea-seam-export",
        "sea-expr-rewriter-test",
        "sea-seam-rules-test",
        "sea-bv-rules-test",
        "sea-addr-range-test",
        "sea-log-test",
        "sea-store-map-test",
        "sea-memops-test",
        "sea-ackermann-test",
        "sea-expr-core-test",
        "sea-expr-api-test",
    ],
    [sea_tools_dir],
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

# Disabled tests carry `REQUIRES: disabled-test` and show up as UNSUPPORTED.
# `lit --param run_disabled=1` runs them anyway (to see if they pass again).
if lit_config.params.get("run_disabled"):
    config.available_features.add("disabled-test")

# The source tree is a git checkout (utils/spdx.py lists its files with git).
if os.path.exists(os.path.join(config.sea_src_root, ".git")) and shutil.which("git"):
    config.available_features.add("sea-git-tree")
