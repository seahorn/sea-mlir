#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Assemble a relocatable Linux install prefix and its dependency evidence."""

import argparse
import csv
import fnmatch
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

# These are part of the supported Ubuntu base, not copied from the builder.
SYSTEM = re.compile(
    r"(?:ld-linux.*|lib(?:c|m|dl|pthread|rt|resolv|util)\.so\..*|"
    r"libgcc_s\.so\..*|libstdc\+\+\.so\..*)$"
)


# LGPL libraries (GMP, GMPXX, MPFR): never shipped (no GPL-family code in a
# release); Ubuntu supplies them: libgmp10 is in the base image, the others
# come from apt. Library name -> Ubuntu package.
APT_RUNTIME = {
    "libgmp.so.10": "libgmp10",
    "libgmpxx.so.4": "libgmpxx4ldbl",
    "libmpfr.so.6": "libmpfr6",
}


def system_library(name):
    return bool(SYSTEM.fullmatch(name)) or name in APT_RUNTIME


# Upstream license files of components built from source and copied into lib/;
# a copied library with neither of these nor a distro package is refused.
UPSTREAM_LICENSE = (
    (re.compile(r"libz3\.so.*"), "Z3-LICENSE.txt"),
    (re.compile(r"libbitwuzla.*\.so.*"), "Bitwuzla-COPYING.txt"),
    (re.compile(r"libyices\.so.*"), "Yices-LICENSE.txt"),
)


def wrap_value(path, key):
    """A value of a Meson .wrap file (Bitwuzla's CaDiCaL and SymFPU pins)."""
    for line in path.read_text().splitlines():
        name, _, value = line.partition("=")
        if name.strip() == key:
            return value.strip()
    raise RuntimeError(f"no {key} in {path}")


def source_pins(wrap_dir):
    """CaDiCaL tag and SymFPU revision, from the wraps Bitwuzla was built with."""
    url = wrap_value(wrap_dir / "cadical.wrap", "source_url")
    tag = re.search(r"/archive/(.+)\.tar\.gz$", url)
    if not tag:
        raise RuntimeError(f"cannot read the CaDiCaL tag from {url}")
    return tag.group(1), wrap_value(wrap_dir / "symfpu.wrap", "revision")


def run(*args, env=None):
    return subprocess.check_output(args, text=True, env=env).strip()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def elf(path):
    if not path.is_file():
        return False
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def merge(source, destination):
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        target = destination / relative
        if path.is_dir() and not path.is_symlink():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() or target.is_symlink():
            if path.is_symlink() and target.is_symlink():
                if path.readlink() == target.readlink():
                    continue
            elif path.is_file() and target.is_file() and digest(path) == digest(target):
                continue
            raise RuntimeError(f"prefix collision: {relative}")
        if path.is_symlink():
            target.symlink_to(path.readlink())
        else:
            shutil.copy2(path, target)


def dependencies(path, env):
    result = subprocess.run(["ldd", str(path)], capture_output=True, text=True, env=env)
    if "not a dynamic executable" in result.stdout + result.stderr:
        return []
    if result.returncode:
        raise RuntimeError(f"ldd failed for {path}: {result.stdout}{result.stderr}")
    found = []
    for line in result.stdout.splitlines():
        if "not found" in line:
            raise RuntimeError(f"missing dependency of {path}: {line}")
        fields = line.split()
        if len(fields) >= 3 and fields[1] == "=>":
            found.append((fields[0], Path(fields[2])))
        elif fields and fields[0].startswith("/"):
            found.append((Path(fields[0]).name, Path(fields[0])))
    return found


def package_owner(path):
    candidates = [path, path.resolve()]
    if str(path).startswith("/lib/"):
        candidates.append(Path("/usr") / path.relative_to("/"))
    for candidate in candidates:
        result = subprocess.run(
            ["dpkg-query", "-S", str(candidate)], capture_output=True, text=True
        )
        if result.returncode == 0:
            return result.stdout.split(": ", 1)[0].splitlines()[0]
    return None


def copy_package_license(package, destination):
    name = package.split(":", 1)[0]
    source = Path("/usr/share/doc") / name / "copyright"
    if not source.is_file():
        raise RuntimeError(f"missing package copyright: {package}")
    target = destination / (name + ".copyright")
    shutil.copyfile(source, target)
    return run("dpkg-query", "-W", "-f=${Version}", package)


def upstream_license(destination, name, url):
    target = destination / name
    with urllib.request.urlopen(url, timeout=60) as response:
        target.write_bytes(response.read())
    if target.stat().st_size < 100:
        raise RuntimeError(f"empty upstream license: {name}")
    return {"file": name, "url": url, "sha256": digest(target)}


def archive(prefix, output, epoch):
    path = output / (prefix.name + ".tar.gz")
    with path.open("wb") as raw:
        with gzip.GzipFile(
            filename="", fileobj=raw, mode="wb", mtime=epoch
        ) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as tar:
                for item in [prefix, *sorted(prefix.rglob("*"))]:
                    info = tar.gettarinfo(
                        str(item), str(item.relative_to(prefix.parent))
                    )
                    info.uid = info.gid = 0
                    info.uname = info.gname = "root"
                    info.mtime = epoch
                    if info.isfile():
                        with item.open("rb") as content:
                            tar.addfile(info, content)
                    else:
                        tar.addfile(info)
    (output / (path.name + ".sha256")).write_text(f"{digest(path)}  {path.name}\n")
    print(f"artifact: {path} bytes={path.stat().st_size} sha256={digest(path)}")


def check_forbidden(prefix):
    if any(path.name.startswith("clang") for path in (prefix / "bin").iterdir()):
        raise RuntimeError("clang must remain an external dependency")


def bundle_libraries(prefix, lib, env):
    """Copy non-system shared libraries into lib/, following dependencies."""
    queue = [path for path in prefix.rglob("*") if not path.is_symlink() and elf(path)]
    visited = set()
    libraries = {}
    packages = {}
    copied = set()
    while queue:
        path = queue.pop()
        if path in visited:
            continue
        visited.add(path)
        for name, source in dependencies(path, env):
            if SYSTEM.fullmatch(name):
                libraries.setdefault(name, {"system": True})
                continue
            if name in APT_RUNTIME:
                libraries.setdefault(name, {"system": True, "apt": APT_RUNTIME[name]})
                continue
            target = lib / name
            if not target.exists():
                shutil.copy2(source.resolve(), target)
                copied.add(name)
                queue.append(target)
            elif target.resolve() != source.resolve() and digest(target) != digest(
                source
            ):
                raise RuntimeError(f"conflicting shared library: {name}")
            entry = libraries.setdefault(name, {"system": False, "source": str(source)})
            owner = package_owner(source)
            if owner:
                entry["package"] = owner
                packages[owner] = None
    return visited, libraries, packages, copied


def require_licenses(libraries, copied):
    """Fail closed: every copied library has a package copyright or an upstream license.
    A library already in the prefix (SeaHorn's copy of libyices) gets its
    upstream license entry too, when it has one."""
    for name, entry in sorted(libraries.items()):
        if name in copied or entry.get("system"):
            continue
        if entry.get("package") or entry.get("license"):
            continue
        for pattern, license_file in UPSTREAM_LICENSE:
            if pattern.fullmatch(name):
                entry["license"] = "share/licenses/release/" + license_file
                break
    for name in sorted(copied):
        entry = libraries[name]
        if entry.get("package") or entry.get("license"):
            continue
        for pattern, license_file in UPSTREAM_LICENSE:
            if pattern.fullmatch(name):
                entry["license"] = "share/licenses/release/" + license_file
                break
        else:
            raise RuntimeError(
                f"no license for copied library {name} ({entry['source']}): "
                "no owning package and no known upstream license"
            )


# License gate. A release ships no GPL-family code (GPL, LGPL, AGPL): every
# shipped file belongs to a component whose license is built from the allowed
# licenses below, or is a license / notice text. An SPDX expression is allowed
# when one of its OR alternatives has only allowed licenses; `X WITH E` counts
# as X (an exception only adds permissions).
ALLOWED_LICENSES = {
    "0BSD",
    "Apache-2.0",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "BSL-1.0",
    "ISC",
    "MIT",
    "NCSA",
    "PSF-2.0",
    "Zlib",
    "LicenseRef-PublicDomain",
}
ALLOWED_EXCEPTIONS = {"LLVM-exception"}

# Components and their licenses. "repo:<name>" means the licenses of that
# repository's REUSE LICENSES/ directory (all of them, ANDead), read at
# packaging time. None: license and notice texts, not code.
COMPONENTS = {
    "notices": None,
    "sea-mlir": "repo:sea-mlir",
    # sea-mlir with LLVM/MLIR at LLVM_REVISION and Boost headers, static
    "sea-mlir-tools": "repo:sea-mlir AND Apache-2.0 WITH LLVM-exception AND BSL-1.0",
    "z3": "MIT",
    # CaDiCaL (MIT) and SymFPU (GPL-3.0 or BSD-3-Clause, at the user's
    # choice) are linked into Bitwuzla
    "bitwuzla": "MIT AND (GPL-3.0-only OR BSD-3-Clause)",
    # built without MCSAT; etc/NOTICES section 1: MiniSat, lookup3, timsort
    "yices": "BSD-3-Clause AND MIT AND LicenseRef-PublicDomain AND PSF-2.0",
    "zlib": "Zlib",
    "zstd": "BSD-3-Clause OR GPL-2.0-only",
    "seahorn": "repo:seahorn",
    "sea-dsa": "repo:sea-dsa",
    # seahorn, sea-dsa, llvm-seahorn, LLVM 18.1.8 and Boost, static
    "seahorn-tools": "repo:seahorn AND repo:sea-dsa AND repo:llvm-seahorn"
    " AND Apache-2.0 WITH LLVM-exception AND BSL-1.0",
    "pyyaml": "MIT",
}

# Shipped path (fnmatch, `*` crosses `/`) -> component; the first match wins.
# A shipped file that matches none fails the build.
FILE_RULES = (
    ("LICENSE", "notices"),
    ("NOTICE", "notices"),
    ("THIRD-PARTY-NOTICES", "notices"),
    ("share/licenses/*", "notices"),
    ("share/release-licenses/*", "notices"),
    ("share/doc/sea-mlir/LICENSE", "notices"),
    ("share/doc/sea-mlir/LICENSES/*", "notices"),
    ("share/doc/seahorn/LICENSE", "notices"),
    ("share/doc/seahorn/LICENSES/*", "notices"),
    ("share/doc/seahorn/NOTICE", "notices"),
    ("share/doc/seahorn/THIRD-PARTY-NOTICES", "notices"),
    ("share/doc/seahorn/*_license.txt", "notices"),
    ("RELEASE-MANIFEST.json", "sea-mlir"),
    ("LLVM_REVISION", "sea-mlir"),
    ("bin/sea-reduce-test", "sea-mlir"),
    ("bin/sea-*", "sea-mlir-tools"),
    ("share/sea-mlir/*", "sea-mlir"),
    ("share/doc/sea-mlir/*", "sea-mlir"),
    ("lib/libz3.so.*", "z3"),
    ("lib/libbitwuzla*.so*", "bitwuzla"),
    ("lib/libyices.so.*", "yices"),
    ("lib/libz.so.*", "zlib"),
    ("lib/libzstd.so.*", "zstd"),
    ("bin/sea", "seahorn"),
    ("bin/stats.py", "seahorn"),
    ("bin/seahorn", "seahorn-tools"),
    ("bin/seapp", "seahorn-tools"),
    ("bin/seaopt", "seahorn-tools"),
    ("bin/seainspect", "seahorn-tools"),
    ("bin/seadsa", "seahorn-tools"),
    ("lib/seapy/yaml/*", "pyyaml"),
    ("lib/seapy/sea/*", "seahorn"),
    ("lib/seapy/term/*", "seahorn"),
    ("include/seahorn/*", "seahorn"),
    ("include/seadsa/*", "sea-dsa"),
    ("lib/libc-64.spec.bc", "sea-dsa"),
    ("lib/sea_dsa.ll", "sea-dsa"),
    ("share/seahorn/profiles/*", "seahorn"),
    ("share/seahorn/build-pins.txt", "seahorn"),
    ("share/seahorn/ldd-builder.txt", "seahorn"),
    ("share/seahorn/runtime-packages.tsv", "seahorn"),
    ("README.md", "seahorn"),
)

# REUSE licenses a repository may list without them reaching a release, with
# the reason.
REPO_LICENSE_EXCEPTIONS = {
    ("seahorn", "LicenseRef-NoLicense"): "play/neha-examples only; not installed",
}

# Ubuntu packages whose libraries may be copied into lib/, with their licenses
PACKAGE_LICENSES = {
    "zlib1g": "Zlib",
    "libzstd1": "BSD-3-Clause OR GPL-2.0-only",
}


# Homebrew bottles (packaging/homebrew/Formula): the same gate over each
# bottle's files, relative to its keg (`<name>/<version>/` stripped), with
# rules per formula. Homebrew's metadata (receipt, SBOM) is not code; the
# formula a bottle carries in .brew/ is this repository's
# (packaging/homebrew/Formula, its SPDX header).
COMPONENTS.update(
    {
        "homebrew-metadata": None,
        "homebrew-formula": "Apache-2.0 WITH LLVM-exception",
        # LLVM + MLIR at LLVM_REVISION, as linked into sea-mlir-tools
        "llvm": "Apache-2.0 WITH LLVM-exception",
    }
)
BOTTLE_COMMON_RULES = (
    ("INSTALL_RECEIPT.json", "homebrew-metadata"),
    ("sbom.spdx.json", "homebrew-metadata"),
    (".brew/*.rb", "homebrew-formula"),
)
BOTTLE_RULES = {
    "sea-mlir-llvm": (
        *BOTTLE_COMMON_RULES,
        ("LICENSE.TXT", "notices"),
        ("share/doc/sea-mlir-llvm/LICENSE.TXT", "notices"),
        ("*", "llvm"),
    ),
    "sea-mlir": (
        *BOTTLE_COMMON_RULES,
        ("LICENSE", "notices"),
        ("share/doc/sea-mlir/LICENSE", "notices"),
        ("share/doc/sea-mlir/LICENSES/*", "notices"),
        ("share/doc/sea-mlir/third-party/*", "notices"),
        ("README.md", "sea-mlir"),
        ("CHANGELOG.md", "sea-mlir"),
        ("AUTHORS", "notices"),
        ("LLVM_REVISION", "sea-mlir"),
        ("bin/sea-reduce-test", "sea-mlir"),
        ("bin/sea-*", "sea-mlir-tools"),
        ("share/sea-mlir/*", "sea-mlir"),
        ("share/doc/sea-mlir/*", "sea-mlir"),
        ("libexec/lib/libz3.so*", "z3"),
        ("libexec/include/z3*.h", "z3"),
        ("libexec/lib/libbitwuzla*.so*", "bitwuzla"),
        ("libexec/include/bitwuzla/*", "bitwuzla"),
        ("libexec/lib/libyices.so*", "yices"),
        ("libexec/include/yices*.h", "yices"),
        ("libexec/bin/yices-smt2", "yices"),
    ),
    "seahorn": (
        *BOTTLE_COMMON_RULES,
        ("share/doc/seahorn/*", "notices"),
        # Homebrew copies these from the source root into the keg
        ("LICENSE", "notices"),
        ("NOTICE", "notices"),
        ("license.txt", "notices"),
        ("README.md", "seahorn"),
        ("bin/sea", "seahorn"),
        ("bin/seahorn", "seahorn-tools"),
        ("bin/seapp", "seahorn-tools"),
        ("bin/seaopt", "seahorn-tools"),
        ("bin/seainspect", "seahorn-tools"),
        ("bin/seadsa", "seahorn-tools"),
        ("libexec/bin/sea", "seahorn"),
        ("libexec/bin/stats.py", "seahorn"),
        ("libexec/bin/seahorn", "seahorn-tools"),
        ("libexec/bin/seapp", "seahorn-tools"),
        ("libexec/bin/seaopt", "seahorn-tools"),
        ("libexec/bin/seainspect", "seahorn-tools"),
        ("libexec/bin/seadsa", "seahorn-tools"),
        ("libexec/lib/seapy/yaml/*", "pyyaml"),
        ("libexec/lib/seapy/*", "seahorn"),
        ("libexec/lib/libc-64.spec.bc", "sea-dsa"),
        ("libexec/lib/sea_dsa.ll", "sea-dsa"),
        ("libexec/include/seahorn/*", "seahorn"),
        ("libexec/include/seadsa/*", "sea-dsa"),
        ("libexec/share/seahorn/profiles/*", "seahorn"),
        ("libexec/share/seahorn/build-pins.txt", "seahorn"),
        ("libexec/share/seahorn.h", "seahorn"),
        ("libexec/share/seahorn.c", "seahorn"),
        ("libexec/README.md", "seahorn"),
        ("libexec/z3/lib/libz3.so*", "z3"),
    ),
}
# where each bottle keeps the REUSE LICENSES/ of the repositories it ships
BOTTLE_REPOS = {
    "sea-mlir-llvm": {},
    "sea-mlir": {"sea-mlir": "share/doc/sea-mlir/LICENSES"},
    "seahorn": {
        name: f"share/doc/seahorn/third-party/{name}-LICENSES"
        for name in ("seahorn", "sea-dsa", "llvm-seahorn")
    },
}


def bottle_paths(bottle):
    """(formula, version, keg-relative paths of the files) of a bottle."""
    paths = []
    keg = None
    with tarfile.open(bottle) as archive:
        for member in archive:
            if member.isdir():
                continue
            parts = member.name.split("/", 2)
            if len(parts) != 3 or not parts[2]:
                raise RuntimeError(f"{bottle.name}: {member.name} is not in a keg")
            if keg is None:
                keg = parts[:2]
            elif parts[:2] != keg:
                raise RuntimeError(f"{bottle.name}: two kegs, {keg} and {parts[:2]}")
            paths.append(parts[2])
    if keg is None:
        raise RuntimeError(f"{bottle.name}: empty bottle")
    return keg[0], keg[1], sorted(paths)


def audit_bottle(bottle):
    """The license gate over one bottle; returns its audit record."""
    name, version, paths = bottle_paths(bottle)
    if name not in BOTTLE_RULES:
        raise RuntimeError(f"{bottle.name}: no license rules for formula {name}")
    expressions = {}
    for repo, directory in BOTTLE_REPOS[name].items():
        found = sorted(
            Path(path).stem
            for path in paths
            if path.startswith(directory + "/")
            and path.count("/") == directory.count("/") + 1
        )
        found = [
            identifier
            for identifier in found
            if identifier not in ALLOWED_EXCEPTIONS
            and (repo, identifier) not in REPO_LICENSE_EXCEPTIONS
        ]
        if not found:
            raise RuntimeError(f"{bottle.name}: no licenses in {directory}")
        expressions[repo] = " AND ".join(found)
    return {
        "formula": name,
        "version": version,
        "files": len(paths),
        "sha256": digest(bottle),
        "components": audit_paths(paths, expressions, BOTTLE_RULES[name]),
    }


def audit_bottles(directory, output):
    bottles = sorted(directory.glob("*.bottle.tar.gz"))
    if not bottles:
        raise RuntimeError(f"no bottles in {directory}")
    report = {bottle.name: audit_bottle(bottle) for bottle in bottles}
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    for name, entry in report.items():
        print(f"license-gate: {name} files={entry['files']} pass")
        for component, record in sorted(entry["components"].items()):
            print(
                f"  {component}: files={record['files']} "
                f"license={record['license']} allowed={record['allowed']}"
            )


def spdx_alternatives(expression):
    """The OR alternatives of an SPDX expression in disjunctive normal form:
    a list of sets of license ids (`X WITH E` as X, E checked separately)."""
    tokens = re.findall(r"\(|\)|[A-Za-z0-9.+:-]+", expression)
    if "".join(tokens) != re.sub(r"\s+", "", expression):
        raise ValueError(f"bad SPDX expression: {expression!r}")
    position = 0

    def peek():
        return tokens[position] if position < len(tokens) else None

    def take():
        nonlocal position
        if position == len(tokens):
            raise ValueError(f"incomplete SPDX expression: {expression!r}")
        position += 1
        return tokens[position - 1]

    def primary():
        token = take()
        if token == "(":
            result = disjunction()
            if take() != ")":
                raise ValueError(f"unbalanced SPDX expression: {expression!r}")
            return result
        if token in ("AND", "OR", "WITH", ")"):
            raise ValueError(f"bad SPDX expression: {expression!r}")
        if peek() == "WITH":
            take()
            exception = take()
            if exception not in ALLOWED_EXCEPTIONS:
                token = f"{token} WITH {exception}"
        return [{token}]

    def conjunction():
        result = primary()
        while peek() == "AND":
            take()
            right = primary()
            result = [left | other for left in result for other in right]
        return result

    def disjunction():
        result = conjunction()
        while peek() == "OR":
            take()
            result = result + conjunction()
        return result

    result = disjunction()
    if position != len(tokens):
        raise ValueError(f"bad SPDX expression: {expression!r}")
    return result


def license_allowed(expression):
    """The allowed alternative of an SPDX expression, or None."""
    for alternative in spdx_alternatives(expression):
        if alternative <= ALLOWED_LICENSES:
            return " AND ".join(sorted(alternative))
    return None


def repo_licenses(name, directory):
    """The licenses of a REUSE repository: its LICENSES/ file names, without
    the listed exceptions, as an AND expression."""
    if not directory.is_dir():
        raise RuntimeError(f"no LICENSES directory for {name}: {directory}")
    found = []
    for path in sorted(directory.iterdir()):
        identifier = path.stem
        if identifier in ALLOWED_EXCEPTIONS:
            continue
        if (name, identifier) in REPO_LICENSE_EXCEPTIONS:
            continue
        found.append(identifier)
    if not found:
        raise RuntimeError(f"no licenses in {directory}")
    return " AND ".join(found)


def resolve_component(component, repos):
    expression = COMPONENTS[component]
    if expression is None:
        return None
    parts = []
    for part in expression.split(" AND "):
        if part.startswith("repo:"):
            parts.extend(repos[part[len("repo:") :]].split(" AND "))
        else:
            parts.append(part)
    return " AND ".join(dict.fromkeys(parts))  # in order, without repeats


def file_component(relative, rules=FILE_RULES):
    for pattern, component in rules:
        if fnmatch.fnmatchcase(relative, pattern):
            return component
    return None


def license_audit(prefix, repos, libraries):
    """Fail closed unless every shipped file has a component with an allowed
    license; return {component: {license, allowed, files}} for the manifest.
    `repos` maps a repository name to its LICENSES/ directory."""
    expressions = {name: repo_licenses(name, path) for name, path in repos.items()}
    paths = [
        path.relative_to(prefix).as_posix()
        for path in sorted(prefix.rglob("*"))
        if not path.is_dir() or path.is_symlink()
    ]
    audit = audit_paths(paths, expressions, FILE_RULES)
    check_copied_packages(libraries)
    return audit


def audit_paths(paths, expressions, rules):
    """The gate over shipped relative paths; `expressions` maps a repository
    to its license expression."""
    audit = {}
    for relative in paths:
        component = file_component(relative, rules)
        if component is None:
            raise RuntimeError(f"no license rule for shipped file {relative}")
        for part in (COMPONENTS[component] or "").split(" AND "):
            if part.startswith("repo:") and part[len("repo:") :] not in expressions:
                raise RuntimeError(
                    f"shipped file {relative}: {part} has no LICENSES directory"
                )
        expression = resolve_component(component, expressions)
        entry = audit.setdefault(
            component, {"license": expression, "allowed": None, "files": 0}
        )
        entry["files"] += 1
        if expression is None:
            continue
        allowed = license_allowed(expression)
        if allowed is None:
            raise RuntimeError(
                f"shipped file {relative}: license {expression} ({component}) "
                "is not built from the allowed licenses (no GPL-family code)"
            )
        entry["allowed"] = allowed
    return audit


def check_copied_packages(libraries):
    for name, entry in sorted(libraries.items()):
        package = entry.get("package")
        if entry.get("system") or not package:
            continue
        package = package.split(":", 1)[0]
        expression = PACKAGE_LICENSES.get(package)
        if expression is None or license_allowed(expression) is None:
            raise RuntimeError(
                f"copied library {name} comes from Ubuntu package {package}, "
                "which has no allowed license entry"
            )


def z3_version(text, revision):
    """The version in `z3 --version` output (a master commit reports one too).
    A z3 built from a git checkout also reports its commit (`build hashcode`);
    a revision given as a full commit must be that one."""
    found = re.search(r"^Z3 version (\S+)", text, re.M)
    if not found:
        raise RuntimeError(f"cannot read the z3 version from: {text!r}")
    if re.fullmatch(r"[0-9a-f]{40}", revision):
        built = re.search(r"build hashcode ([0-9a-f]{40})", text)
        if not built or built.group(1) != revision:
            raise RuntimeError(
                f"z3 reports {built.group(1) if built else 'no commit'}, "
                f"expected the pin {revision}"
            )
    return found.group(1)


def check_solvers(version_text, z3, yices):
    """`sea-vc --version` must report the pinned z3 and Yices: a release
    without Yices, or with other solver versions than the pins, is refused."""
    lines = [line.strip() for line in version_text.splitlines()]
    for name, version in (("z3", z3), ("yices", yices)):
        # `yices: not built (...)` counts as the yices line
        found = [line for line in lines if line.split(" ", 1)[0].rstrip(":") == name]
        if not found or not re.fullmatch(
            rf"{name} {re.escape(version)}(\.0)*", found[0]
        ):
            reported = repr(found[0]) if found else f"no {name} line"
            raise RuntimeError(f"sea-vc reports {reported}, expected {name} {version}")


def yices_legal(tag):
    """Yices' legal files for a binary distribution: its license, the
    third-party notices (etc/NOTICES, section 1 covers every binary: MiniSat
    and CPython timsort material) and the license texts NOTICES names for
    them; (file name in the release, upstream URL)."""
    base = f"https://raw.githubusercontent.com/SRI-CSL/yices2/{tag}"
    return [
        ("Yices-LICENSE.txt", f"{base}/LICENSE.txt"),
        ("Yices-NOTICES.txt", f"{base}/etc/NOTICES"),
        (
            "Yices-CPython-3.4.0a3-LICENSE.txt",
            f"{base}/LICENSES/CPython-3.4.0a3-LICENSE.txt",
        ),
        ("Yices-MIT.txt", f"{base}/LICENSES/MIT.txt"),
    ]


def set_runpaths(prefix, lib, visited):
    # Every object gets its own relative search path; RUNPATH is not transitive.
    for path in sorted(visited):
        needed = subprocess.run(
            ["patchelf", "--print-needed", str(path)], capture_output=True
        )
        if needed.returncode:
            continue  # Static executables have no dynamic loader metadata.
        relative = os.path.relpath(lib, path.parent)
        rpath = "$ORIGIN" if relative == "." else "$ORIGIN/" + relative
        subprocess.run(["patchelf", "--set-rpath", rpath, str(path)], check=True)
        if run("patchelf", "--print-rpath", str(path)) != rpath:
            raise RuntimeError(f"incorrect relative runpath: {path}")


def check_relocatable(prefix, visited, env):
    elf_report = {}
    for path in sorted(visited):
        deps = dependencies(path, env)
        for name, resolved in deps:
            if not system_library(name) and not resolved.is_relative_to(prefix):
                raise RuntimeError(f"nonrelocatable dependency: {path}: {resolved}")
        elf_report[str(path.relative_to(prefix))] = {
            "sha256": digest(path),
            "dependencies": {name: str(resolved) for name, resolved in deps},
        }
    return elf_report


def main():
    if "--audit-bottles" in sys.argv[1:]:
        bottles = argparse.ArgumentParser(description="license gate over bottles")
        bottles.add_argument("--audit-bottles", type=Path, required=True)
        bottles.add_argument("--output", type=Path, required=True)
        args = bottles.parse_args()
        audit_bottles(args.audit_bottles, args.output)
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True)
    parser.add_argument("--source-revision", default="unknown")
    parser.add_argument("--seahorn", type=Path)
    parser.add_argument(
        "--z3-revision", required=True, help="the commit or tag z3 was built from"
    )
    parser.add_argument("--bitwuzla-tag", required=True)
    parser.add_argument("--yices-tag", required=True)
    parser.add_argument(
        "--z3",
        type=Path,
        default=Path("/opt/solvers/bin/z3"),
        help="the z3 binary of the solvers stage (its version for the notices)",
    )
    parser.add_argument(
        "--wrap-dir",
        type=Path,
        default=Path("/opt/solvers/share/pins"),
        help="Bitwuzla's cadical.wrap and symfpu.wrap, saved by the solvers stage",
    )
    parser.add_argument(
        "--sea-mlir-source",
        type=Path,
        default=Path("/src/sea-mlir"),
        help="the sea-mlir source tree (its REUSE LICENSES/ for the license gate)",
    )
    parser.add_argument("--output", type=Path, default=Path("/artifacts"))
    parser.add_argument("--release-root", type=Path, default=Path("/release"))
    parser.add_argument(
        "--epoch", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    )
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.name):
        parser.error("name must be a plain package name")
    prefix = args.release_root / args.name
    prefix.mkdir(parents=True, exist_ok=False)
    merge(Path("/opt/sea-mlir"), prefix)
    if args.seahorn:
        merge(args.seahorn, prefix)
    for path in sorted(prefix.rglob("*.a")):
        path.unlink()
    for metadata in (prefix / "lib" / "cmake", prefix / "lib" / "pkgconfig"):
        if metadata.is_dir():
            shutil.rmtree(metadata)
    for path in prefix.rglob("*"):
        if path.is_symlink() and not path.resolve().is_relative_to(prefix):
            raise RuntimeError(f"nonrelocatable symlink: {path}")
    lib = prefix / "lib"
    lib.mkdir(exist_ok=True)
    check_forbidden(prefix)

    search = [lib, Path("/opt/solvers/lib")]
    if args.seahorn:
        search.insert(1, args.seahorn / "lib")
    env = dict(os.environ, LD_LIBRARY_PATH=":".join(map(str, search)))
    visited, libraries, packages, copied = bundle_libraries(prefix, lib, env)

    seahorn_packages = []
    provenance = prefix / "share/seahorn/runtime-packages.tsv"
    if args.seahorn:
        with provenance.open() as stream:
            for row in csv.DictReader(stream, delimiter="\t"):
                license_path = prefix / row["license"]
                if (
                    not license_path.resolve().is_relative_to(prefix)
                    or not license_path.is_file()
                ):
                    raise RuntimeError(
                        f"missing SeaHorn runtime license: {license_path}"
                    )
                seahorn_packages.append(row)
                if row["library"] in libraries:
                    libraries[row["library"]].update(
                        {
                            "original_source": row["original_path"],
                            "package": row["package"],
                            "version": row["version"],
                            "license": row["license"],
                        }
                    )

    if args.seahorn and "libz3.so.4.12" in libraries:
        # SeaHorn's z3 4.12.0, preinstalled by the SeaHorn stage
        z3_seahorn = "share/release-licenses/z3-seahorn.txt"
        if not (prefix / z3_seahorn).is_file():
            raise RuntimeError(f"missing SeaHorn z3 license: {z3_seahorn}")
        libraries["libz3.so.4.12"]["license"] = z3_seahorn
    require_licenses(libraries, copied)
    license_dir = prefix / "share" / "licenses" / "release"
    license_dir.mkdir(parents=True)
    llvm_pin = Path("/opt/llvm/LLVM_REVISION").read_text()
    llvm_commit = re.search(r"^commit:\s*(\S+)", llvm_pin, re.M).group(1)
    cadical_tag, symfpu_revision = source_pins(args.wrap_dir)
    z3 = z3_version(run(str(args.z3), "--version"), args.z3_revision)
    z3_short = args.z3_revision
    if re.fullmatch(r"[0-9a-f]{40}", z3_short):
        z3_short = "commit " + z3_short[:7]
    yices = args.yices_tag.removeprefix("yices-")
    cadical_version = cadical_tag.removeprefix("rel-")
    licenses = [
        upstream_license(
            license_dir,
            "LLVM-LICENSE.txt",
            f"https://raw.githubusercontent.com/llvm/llvm-project/{llvm_commit}/llvm/LICENSE.TXT",
        ),
        upstream_license(
            license_dir,
            "Z3-LICENSE.txt",
            f"https://raw.githubusercontent.com/Z3Prover/z3/{args.z3_revision}/LICENSE.txt",
        ),
        upstream_license(
            license_dir,
            "Bitwuzla-COPYING.txt",
            f"https://raw.githubusercontent.com/bitwuzla/bitwuzla/{args.bitwuzla_tag}/COPYING",
        ),
        *(
            upstream_license(license_dir, name, url)
            for name, url in yices_legal(args.yices_tag)
        ),
        upstream_license(
            license_dir,
            "CaDiCaL-LICENSE.txt",
            f"https://raw.githubusercontent.com/arminbiere/cadical/{cadical_tag}/LICENSE",
        ),
        upstream_license(
            license_dir,
            "SymFPU-LICENSE-BSD.txt",
            f"https://raw.githubusercontent.com/martin-cs/symfpu/{symfpu_revision}/LICENSE-BSD",
        ),
    ]
    if args.seahorn:
        # SeaHorn's tools link LLVM 18.1.8 statically (apt.llvm.org); its
        # Debian copyright file is share/release-licenses/llvm-18.txt
        licenses.append(
            upstream_license(
                license_dir,
                "LLVM-18-LICENSE.txt",
                "https://raw.githubusercontent.com/llvm/llvm-project/"
                "llvmorg-18.1.8/llvm/LICENSE.TXT",
            )
        )
    packages["libboost1.83-dev"] = None
    for package in packages:
        packages[package] = copy_package_license(package, license_dir)
    runtime_packages = sorted(
        {entry["apt"] for entry in libraries.values() if entry.get("apt")}
    )
    shutil.copyfile(prefix / "share/doc/sea-mlir/LICENSE", prefix / "LICENSE")
    shutil.copytree(
        args.sea_mlir_source / "LICENSES", prefix / "share/doc/sea-mlir/LICENSES"
    )
    shutil.copyfile("/opt/llvm/LLVM_REVISION", prefix / "LLVM_REVISION")

    set_runpaths(prefix, lib, visited)
    clean_env = dict(os.environ)
    clean_env.pop("LD_LIBRARY_PATH", None)
    elf_report = check_relocatable(prefix, visited, clean_env)
    sea_vc_version = run(str(prefix / "bin/sea-vc"), "--version", env=clean_env)
    check_solvers(sea_vc_version, z3, yices)

    apt_line = " ".join(runtime_packages) or "(none)"
    text = f"""Third-party components in this install prefix

sea-mlir: Apache-2.0 WITH LLVM-exception; LICENSE at the prefix root.
LLVM/MLIR: Apache-2.0 WITH LLVM-exception; pin in LLVM_REVISION.
Z3 {z3} ({z3_short}): MIT. Bitwuzla {args.bitwuzla_tag}: MIT.
Yices {yices}: BSD 3-Clause; built without MCSAT, so LibPoly is not linked.
  Its third-party notices (MiniSat, CPython timsort) are in Yices-NOTICES.txt,
  Yices-MIT.txt and Yices-CPython-3.4.0a3-LICENSE.txt.
CaDiCaL {cadical_version}: MIT; linked into Bitwuzla.
SymFPU {symfpu_revision[:7]}: BSD 3-Clause option; linked into Bitwuzla.
Boost: Boost Software License; headers used by sea-mlir.
zlib and Zstandard: their package copyright/license texts are included.

Full license texts and package copyrights: share/licenses/release/.
This prefix contains no GPL-family (GPL, LGPL, AGPL) code: GMP, GMPXX and MPFR
(LGPL) are not included. Ubuntu supplies them: libgmp10 is in the base image;
install the others with `apt-get install {apt_line}`.
Ubuntu 24.04 also supplies glibc, libstdc++, libgcc and its dynamic loader.
Compiler and solver source pins, dependency versions and hashes are recorded
in RELEASE-MANIFEST.json. Obtain Ubuntu library sources with apt source for
the recorded package/version; upstream source URLs are in the license records.

The ELF tools need Ubuntu 24.04 x86_64 or a compatible newer runtime.
Python utilities require Python 3. The SeaHorn frontend also needs clang-18.
"""
    seahorn_pins = {}
    if args.seahorn:
        text += """
SeaHorn, sea-dsa and llvm-seahorn (Apache-2.0 WITH LLVM-exception): their
license and notice files at the pinned commits are under
share/release-licenses/ (<repository>-LICENSE, ...); the commits are in
RELEASE-MANIFEST.json ("seahorn"). SeaHorn is built without clam/crab.
SeaHorn links z3 4.12.0 (MIT, lib/libz3.so.4.12), a second z3 next to the one
sea-mlir uses, and the same Yices library as sea-vc (lib/libyices.so.2.8).
LLVM 18.1.8 is linked statically into the SeaHorn tools: LLVM-18-LICENSE.txt
(upstream) and share/release-licenses/llvm-18.txt (Debian copyright).
PyYAML (MIT) is included for the `sea` driver.
"""
        for line in (prefix / "share/seahorn/build-pins.txt").read_text().splitlines():
            key, _, value = line.partition(":")
            seahorn_pins[key.strip()] = value.strip()
    (prefix / "THIRD-PARTY-NOTICES").write_text(text)
    (prefix / "NOTICE").write_text(
        "sea-mlir\nCopyright 2026 The sea-mlir Authors\nSee LICENSE and THIRD-PARTY-NOTICES for terms and dependency notices.\n"
    )
    repos = {"sea-mlir": prefix / "share/doc/sea-mlir/LICENSES"}
    if args.seahorn:
        for name in ("seahorn", "sea-dsa", "llvm-seahorn"):
            repos[name] = prefix / "share/release-licenses" / (name + "-LICENSES")
    audit = license_audit(prefix, repos, libraries)
    manifest = {
        "name": args.name,
        "source_revision": args.source_revision,
        "llvm_commit": llvm_commit,
        "seahorn": seahorn_pins,
        "runtime_packages": runtime_packages,
        "license_audit": audit,
        "solvers": {
            "z3": {"revision": args.z3_revision, "version": z3},
            "bitwuzla": {"tag": args.bitwuzla_tag},
            "yices": {"tag": args.yices_tag, "version": yices},
        },
        "licenses": licenses,
        "packages": packages,
        "seahorn_runtime_packages": seahorn_packages,
        "libraries": libraries,
        "elf": elf_report,
        "sea_vc_version": sea_vc_version,
    }
    (prefix / "RELEASE-MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    args.output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(
        prefix / "RELEASE-MANIFEST.json", args.output / (args.name + ".json")
    )
    archive(prefix, args.output, args.epoch)


if __name__ == "__main__":
    main()
