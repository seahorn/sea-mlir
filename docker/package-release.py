#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Assemble a relocatable Linux install prefix and its dependency evidence."""

import argparse
import csv
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path

# These are part of the supported Ubuntu base, not copied from the builder.
SYSTEM = re.compile(
    r"(?:ld-linux.*|lib(?:c|m|dl|pthread|rt|resolv|util)\.so\..*|"
    r"libgcc_s\.so\..*|libstdc\+\+\.so\..*)$"
)


# Upstream license files of components built from source and copied into lib/;
# a copied library with neither of these nor a distro package is refused.
UPSTREAM_LICENSE = (
    (re.compile(r"libz3\.so.*"), "Z3-LICENSE.txt"),
    (re.compile(r"libbitwuzla.*\.so.*"), "Bitwuzla-COPYING.txt"),
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


def archive(prefix, output, epoch, suffix=""):
    path = output / (prefix.name + suffix + ".tar.gz")
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
    for path in prefix.rglob("*"):
        if "yices" in path.name.lower() and (
            path.parent == prefix / "bin" or ".so" in path.name or path.suffix == ".a"
        ):
            raise RuntimeError(f"Yices binary is forbidden in a release: {path}")
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
            if "yices" in name.lower():
                raise RuntimeError("Yices dependency is forbidden")
            if SYSTEM.fullmatch(name):
                libraries.setdefault(name, {"system": True})
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
    """Fail closed: every copied library has a package copyright or an upstream license."""
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
            if not SYSTEM.fullmatch(name) and not resolved.is_relative_to(prefix):
                raise RuntimeError(f"nonrelocatable dependency: {path}: {resolved}")
        elf_report[str(path.relative_to(prefix))] = {
            "sha256": digest(path),
            "dependencies": {name: str(resolved) for name, resolved in deps},
        }
    return elf_report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True)
    parser.add_argument("--source-revision", default="unknown")
    parser.add_argument("--seahorn", type=Path)
    parser.add_argument("--z3-tag", required=True)
    parser.add_argument("--bitwuzla-tag", required=True)
    parser.add_argument(
        "--wrap-dir",
        type=Path,
        default=Path("/opt/solvers/share/pins"),
        help="Bitwuzla's cadical.wrap and symfpu.wrap, saved by the solvers stage",
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

    require_licenses(libraries, copied)
    license_dir = prefix / "share" / "licenses" / "release"
    license_dir.mkdir(parents=True)
    llvm_pin = Path("/opt/llvm/LLVM_REVISION").read_text()
    llvm_commit = re.search(r"^commit:\s*(\S+)", llvm_pin, re.M).group(1)
    cadical_tag, symfpu_revision = source_pins(args.wrap_dir)
    z3_version = args.z3_tag.removeprefix("z3-")
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
            f"https://raw.githubusercontent.com/Z3Prover/z3/{args.z3_tag}/LICENSE.txt",
        ),
        upstream_license(
            license_dir,
            "Bitwuzla-COPYING.txt",
            f"https://raw.githubusercontent.com/bitwuzla/bitwuzla/{args.bitwuzla_tag}/COPYING",
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
    packages["libboost1.83-dev"] = None
    for package in packages:
        packages[package] = copy_package_license(package, license_dir)
    for name in ("LGPL-2.1", "LGPL-3", "GPL-3"):
        shutil.copyfile(Path("/usr/share/common-licenses") / name, license_dir / name)
    shutil.copyfile(prefix / "share/doc/sea-mlir/LICENSE", prefix / "LICENSE")
    shutil.copyfile("/opt/llvm/LLVM_REVISION", prefix / "LLVM_REVISION")

    set_runpaths(prefix, lib, visited)
    clean_env = dict(os.environ)
    clean_env.pop("LD_LIBRARY_PATH", None)
    elf_report = check_relocatable(prefix, visited, clean_env)

    text = f"""Third-party components in this install prefix

sea-mlir: Apache-2.0 WITH LLVM-exception; LICENSE at the prefix root.
LLVM/MLIR: Apache-2.0 WITH LLVM-exception; pin in LLVM_REVISION.
Z3 {z3_version}: MIT. Bitwuzla {args.bitwuzla_tag}: MIT.
CaDiCaL {cadical_version}: MIT; linked into Bitwuzla.
SymFPU {symfpu_revision[:7]}: BSD 3-Clause option; linked into Bitwuzla.
Boost: Boost Software License; headers used by sea-mlir.
GMP/GMPXX and MPFR: LGPL; dynamically linked, replaceable libraries in lib/.
zlib and Zstandard: their package copyright/license texts are included.

Full license texts and package copyrights: share/licenses/release/.
The included GPL text is incorporated by LGPL-3's terms; it does not indicate
that Yices or a GPL-only solver is included. Yices is not present.
Ubuntu 24.04 supplies glibc, libstdc++, libgcc and its dynamic loader.
Compiler and solver source pins, dependency versions and hashes are recorded
in RELEASE-MANIFEST.json. Obtain Ubuntu library sources with apt source for
the recorded package/version; upstream source URLs are in the license records.

The ELF tools need Ubuntu 24.04 x86_64 or a compatible newer runtime.
Python utilities require Python 3. The SeaHorn frontend also needs clang-18.
"""
    if args.seahorn:
        text += """
SEAHORN BUNDLE: NOT FOR DISTRIBUTION; SEAHORN IS TO BE RELICENSED.
SeaHorn's current license and dependency licenses are retained under
share/release-licenses/ (seahorn.txt, sea-dsa.txt, llvm-seahorn.txt, ...).
This local bundle is for build and interoperability testing only until the
maintainer relicenses it.
"""
        (prefix / "NOT-FOR-DISTRIBUTION").write_text(
            "SeaHorn is to be relicensed before this bundle can ship.\n"
        )
    (prefix / "THIRD-PARTY-NOTICES").write_text(text)
    (prefix / "NOTICE").write_text(
        "sea-mlir\nCopyright 2026 The sea-mlir Authors\nSee LICENSE and THIRD-PARTY-NOTICES for terms and dependency notices.\n"
    )
    suffix = "-held" if args.seahorn else ""
    manifest = {
        "name": args.name,
        "source_revision": args.source_revision,
        "distribution_hold": bool(args.seahorn),
        "llvm_commit": llvm_commit,
        "licenses": licenses,
        "packages": packages,
        "seahorn_runtime_packages": seahorn_packages,
        "libraries": libraries,
        "elf": elf_report,
        "sea_vc_version": run(str(prefix / "bin/sea-vc"), "--version", env=clean_env),
    }
    (prefix / "RELEASE-MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    args.output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(
        prefix / "RELEASE-MANIFEST.json", args.output / (args.name + suffix + ".json")
    )
    archive(prefix, args.output, args.epoch, suffix)


if __name__ == "__main__":
    main()
