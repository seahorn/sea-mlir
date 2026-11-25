# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Guards of package-release.py on fake prefixes; no Docker, no real ldd/patchelf."""

import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "package_release", Path(__file__).with_name("package-release.py")
)
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)

ELF = b"\x7fELF" + bytes(60)


class PrefixCase(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.prefix = self.root / "prefix"
        (self.prefix / "bin").mkdir(parents=True)
        (self.prefix / "lib").mkdir()
        self.tool = self.prefix / "bin" / "sea-vc"
        self.tool.write_bytes(ELF)
        self.outside = self.root / "builder"
        self.outside.mkdir()

    def library(self, name, content=ELF):
        path = self.outside / name
        path.write_bytes(content)
        return path


class ForbiddenTests(PrefixCase):
    def test_clean_prefix_passes(self):
        pr.check_forbidden(self.prefix)

    def test_yices_allowed(self):
        # Yices 2.8.0 is BSD-3-Clause: release binaries include it
        (self.prefix / "lib" / "libyices.so.2.8").write_bytes(ELF)
        (self.prefix / "bin" / "yices-smt2").write_bytes(ELF)
        pr.check_forbidden(self.prefix)

    def test_clang_refused(self):
        (self.prefix / "bin" / "clang-18").write_bytes(ELF)
        with self.assertRaisesRegex(RuntimeError, "clang"):
            pr.check_forbidden(self.prefix)


class LicenseTests(PrefixCase):
    def bundle(self, name, owner=None):
        deps = [(name, self.library(name))]
        lib = self.prefix / "lib"
        with (
            patch.object(
                pr,
                "dependencies",
                side_effect=lambda p, e: deps if p == self.tool else [],
            ),
            patch.object(pr, "package_owner", return_value=owner),
        ):
            return pr.bundle_libraries(self.prefix, lib, {})

    def test_unlicensed_copied_library_refused(self):
        _, libraries, _, copied = self.bundle("libmystery.so.1")
        self.assertEqual(copied, {"libmystery.so.1"})
        with self.assertRaisesRegex(RuntimeError, "no license.*libmystery"):
            pr.require_licenses(libraries, copied)

    def test_owned_library_accepted(self):
        _, libraries, packages, copied = self.bundle("libz.so.1", "zlib1g:amd64")
        self.assertIn("zlib1g:amd64", packages)
        pr.require_licenses(libraries, copied)

    def test_lgpl_library_not_copied(self):
        # GMP, GMPXX, MPFR (LGPL) stay Ubuntu runtime packages
        for name, package in pr.APT_RUNTIME.items():
            _, libraries, packages, copied = self.bundle(name, package + ":amd64")
            self.assertEqual(copied, set())
            self.assertEqual(packages, {})
            self.assertEqual(libraries[name], {"system": True, "apt": package})
            self.assertFalse((self.prefix / "lib" / name).exists())

    def test_known_upstream_library_accepted(self):
        _, libraries, _, copied = self.bundle("libz3.so.5.3")
        pr.require_licenses(libraries, copied)
        self.assertTrue(libraries["libz3.so.5.3"]["license"].endswith("Z3-LICENSE.txt"))

    def test_yices_library_has_upstream_license(self):
        _, libraries, _, copied = self.bundle("libyices.so.2.8")
        pr.require_licenses(libraries, copied)
        self.assertTrue(
            libraries["libyices.so.2.8"]["license"].endswith("Yices-LICENSE.txt")
        )

    def test_yices_ships_notices(self):
        # NOTICES section 1 (MiniSat, CPython timsort) covers every binary
        legal = dict(pr.yices_legal("yices-2.8.0"))
        self.assertEqual(
            set(legal),
            {
                "Yices-LICENSE.txt",
                "Yices-NOTICES.txt",
                "Yices-CPython-3.4.0a3-LICENSE.txt",
                "Yices-MIT.txt",
            },
        )
        self.assertTrue(legal["Yices-NOTICES.txt"].endswith("/yices-2.8.0/etc/NOTICES"))
        self.assertTrue(all("/yices-2.8.0/" in url for url in legal.values()))
        # the library's license entry is one of the shipped files
        _, libraries, _, copied = self.bundle("libyices.so.2.8")
        pr.require_licenses(libraries, copied)
        self.assertIn(libraries["libyices.so.2.8"]["license"].rsplit("/", 1)[1], legal)

    def test_library_with_manifest_license_accepted(self):
        _, libraries, _, copied = self.bundle("libother.so.1")
        libraries["libother.so.1"]["license"] = "share/release-licenses/x.txt"
        pr.require_licenses(libraries, copied)

    def test_preinstalled_library_is_not_checked(self):
        (self.prefix / "lib" / "libown.so").write_bytes(ELF)
        deps = [("libown.so", self.prefix / "lib" / "libown.so")]
        with (
            patch.object(
                pr,
                "dependencies",
                side_effect=lambda p, e: deps if p == self.tool else [],
            ),
            patch.object(pr, "package_owner", return_value=None),
        ):
            _, libraries, _, copied = pr.bundle_libraries(
                self.prefix, self.prefix / "lib", {}
            )
        self.assertEqual(copied, set())
        pr.require_licenses(libraries, copied)

    def test_preinstalled_upstream_library_gets_license(self):
        # SeaHorn's copy of libyices.so.2.8 is in the prefix before packaging
        libraries = {"libyices.so.2.8": {"system": False, "source": "lib"}}
        pr.require_licenses(libraries, set())
        self.assertTrue(
            libraries["libyices.so.2.8"]["license"].endswith("Yices-LICENSE.txt")
        )


class RelocatableTests(PrefixCase):
    def test_dependency_outside_prefix_refused(self):
        deps = [("libfoo.so.1", self.library("libfoo.so.1"))]
        with (
            patch.object(pr, "dependencies", return_value=deps),
            self.assertRaisesRegex(RuntimeError, "nonrelocatable dependency"),
        ):
            pr.check_relocatable(self.prefix, {self.tool}, {})

    def test_inside_and_system_dependencies_accepted(self):
        inside = self.prefix / "lib" / "libfoo.so.1"
        inside.write_bytes(ELF)
        deps = [
            ("libfoo.so.1", inside),
            ("libc.so.6", Path("/lib/x86_64-linux-gnu/libc.so.6")),
        ]
        with patch.object(pr, "dependencies", return_value=deps):
            report = pr.check_relocatable(self.prefix, {self.tool}, {})
        self.assertEqual(list(report), ["bin/sea-vc"])

    def stub_patchelf(self, printed):
        def fake(command, **kwargs):
            return subprocess.CompletedProcess(command, 0, b"", b"")

        return (
            patch.object(pr.subprocess, "run", side_effect=fake),
            patch.object(pr, "run", return_value=printed),
        )

    def test_apt_runtime_dependency_accepted(self):
        deps = [("libmpfr.so.6", Path("/lib/x86_64-linux-gnu/libmpfr.so.6"))]
        with patch.object(pr, "dependencies", return_value=deps):
            pr.check_relocatable(self.prefix, {self.tool}, {})

    def test_wrong_runpath_refused(self):
        run_patch, print_patch = self.stub_patchelf("/opt/solvers/lib")
        with run_patch, print_patch, self.assertRaisesRegex(RuntimeError, "runpath"):
            pr.set_runpaths(self.prefix, self.prefix / "lib", {self.tool})

    def test_relative_runpath_accepted(self):
        run_patch, print_patch = self.stub_patchelf("$ORIGIN/../lib")
        with run_patch as run, print_patch:
            pr.set_runpaths(self.prefix, self.prefix / "lib", {self.tool})
        self.assertIn("$ORIGIN/../lib", run.call_args_list[-1].args[0])

    def test_missing_dependency_refused(self):
        result = subprocess.CompletedProcess(
            ["ldd"], 0, "\tlibfoo.so.1 => not found\n", ""
        )
        with (
            patch.object(pr.subprocess, "run", return_value=result),
            self.assertRaisesRegex(RuntimeError, "missing dependency"),
        ):
            pr.dependencies(self.tool, {})


VERSION = """sea-vc (sea-mlir 0.5.0 (v0.5.0))
  LLVM/MLIR 24.0.0git at 302c842d6601
  build: Release, assertions on
  z3 5.4.2.0
  yices 2.8.0
  bitwuzla 0.9.1
"""


class SolverTests(unittest.TestCase):
    def test_z3_version(self):
        commit = "bbd46c9ebdd5955333682fe9f3cefc2b7c883e9c"
        text = "Z3 version 5.4.2 - 64 bit - build hashcode " + commit + "\n"
        self.assertEqual(pr.z3_version(text, commit), "5.4.2")
        self.assertEqual(
            pr.z3_version("Z3 version 5.3.3 - 64 bit\n", "z3-5.3.3"), "5.3.3"
        )
        with self.assertRaisesRegex(RuntimeError, "z3 version"):
            pr.z3_version("z3: command not found", "z3-5.3.3")

    def test_z3_other_commit_refused(self):
        commit = "bbd46c9ebdd5955333682fe9f3cefc2b7c883e9c"
        with self.assertRaisesRegex(RuntimeError, "expected the pin"):
            pr.z3_version(
                "Z3 version 5.4.2 - 64 bit - build hashcode " + "0" * 40, commit
            )
        with self.assertRaisesRegex(RuntimeError, "no commit"):
            pr.z3_version("Z3 version 5.4.2 - 64 bit\n", commit)

    def test_pinned_solvers_accepted(self):
        pr.check_solvers(VERSION, "5.4.2", "2.8.0")

    def test_release_without_yices_refused(self):
        text = VERSION.replace(
            "yices 2.8.0", "yices: not built (--solver=yices disabled)"
        )
        with self.assertRaisesRegex(
            RuntimeError, "reports 'yices: not built.*expected yices 2.8.0"
        ):
            pr.check_solvers(text, "5.4.2", "2.8.0")

    def test_other_solver_version_refused(self):
        with self.assertRaisesRegex(RuntimeError, "expected z3 5.3.3"):
            pr.check_solvers(VERSION, "5.3.3", "2.8.0")
        with self.assertRaisesRegex(RuntimeError, "expected yices 2.8"):
            pr.check_solvers(VERSION.replace("2.8.0", "2.8.10"), "5.4.2", "2.8.0")


class PinTests(PrefixCase):
    def test_pins_come_from_wraps(self):
        wraps = self.root / "pins"
        wraps.mkdir()
        (wraps / "cadical.wrap").write_text(
            "[wrap-file]\nsource_url = https://github.com/arminbiere/cadical/archive/rel-9.9.9.tar.gz\n"
        )
        (wraps / "symfpu.wrap").write_text("[wrap-git]\nrevision = abcdef0123\n")
        self.assertEqual(pr.source_pins(wraps), ("rel-9.9.9", "abcdef0123"))

    def test_missing_wrap_refused(self):
        with self.assertRaises(OSError):
            pr.source_pins(self.root / "none")


if __name__ == "__main__":
    unittest.main()


class SpdxTests(unittest.TestCase):
    def test_permissive_expressions_allowed(self):
        for expression in (
            "MIT",
            "Apache-2.0 WITH LLVM-exception",
            "Apache-2.0 WITH LLVM-exception AND BSL-1.0",
            "BSD-3-Clause OR GPL-2.0-only",
            "MIT AND (GPL-3.0-only OR BSD-3-Clause)",
        ):
            self.assertIsNotNone(pr.license_allowed(expression), expression)

    def test_gpl_family_refused(self):
        for expression in (
            "GPL-2.0-only",
            "LGPL-3.0-or-later",
            "LGPL-3.0-or-later OR GPL-2.0-or-later",
            "AGPL-3.0-only",
            "MIT AND GPL-3.0-or-later",
            # an exception does not make a GPL license allowed
            "GPL-3.0-or-later WITH GCC-exception-3.1",
            # an unknown exception is not dropped
            "MIT WITH Unknown-exception",
        ):
            self.assertIsNone(pr.license_allowed(expression), expression)

    def test_malformed_refused(self):
        for expression in ("MIT AND", "(MIT", "MIT OR OR BSD-3-Clause", "MIT; rm"):
            with self.assertRaises(ValueError, msg=expression):
                pr.license_allowed(expression)


class LicenseGateTests(unittest.TestCase):
    """The license gate on fake release prefixes."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.prefix = Path(temporary.name) / "prefix"
        for relative in (
            "bin/sea-vc",
            "bin/sea-reduce-test",
            "lib/libz3.so.5.4",
            "lib/libyices.so.2.8",
            "lib/libbitwuzla.so.0",
            "lib/libzstd.so.1",
            "share/sea-mlir/profiles/vcc.yaml",
            "share/doc/sea-mlir/README.md",
            "share/licenses/release/GPL-text-of-a-dual-license",
            "LICENSE",
            "THIRD-PARTY-NOTICES",
            "RELEASE-MANIFEST.json",
        ):
            self.add(relative)
        self.repos = {
            "sea-mlir": self.licenses("sea-mlir", "Apache-2.0", "LLVM-exception", "MIT")
        }

    def add(self, relative):
        path = self.prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(ELF)

    def licenses(self, name, *identifiers):
        directory = self.prefix / "share/release-licenses" / (name + "-LICENSES")
        directory.mkdir(parents=True, exist_ok=True)
        for identifier in identifiers:
            (directory / (identifier + ".txt")).write_text("text")
        return directory

    def seahorn(self):
        for relative in (
            "bin/sea",
            "bin/seahorn",
            "lib/seapy/sea/commands.py",
            "lib/seapy/yaml/__init__.py",
            "include/seadsa/Graph.hh",
            "share/seahorn/profiles/sea-mlir-export.yaml",
            "lib/libz3.so.4.12",
        ):
            self.add(relative)
        self.repos["seahorn"] = self.licenses(
            "seahorn", "Apache-2.0", "LLVM-exception", "MIT", "LicenseRef-NoLicense"
        )
        self.repos["sea-dsa"] = self.licenses("sea-dsa", "Apache-2.0", "NCSA")
        self.repos["llvm-seahorn"] = self.licenses("llvm-seahorn", "Apache-2.0")

    def audit(self, libraries=None):
        return pr.license_audit(self.prefix, self.repos, libraries or {})

    def test_release_passes(self):
        audit = self.audit()
        self.assertEqual(audit["sea-mlir"]["license"], "Apache-2.0 AND MIT")
        self.assertEqual(audit["yices"]["files"], 1)
        self.assertIsNone(audit["notices"]["license"])

    def test_bundle_passes(self):
        self.seahorn()
        audit = self.audit()
        # LicenseRef-NoLicense (play/, not installed) is excepted
        self.assertNotIn("NoLicense", audit["seahorn"]["license"])
        self.assertIn("NCSA", audit["seahorn-tools"]["license"])

    def test_every_component_is_allowed(self):
        repos = {
            name: "Apache-2.0"
            for name in ("sea-mlir", "seahorn", "sea-dsa", "llvm-seahorn")
        }
        for component in pr.COMPONENTS:
            expression = pr.resolve_component(component, repos)
            if expression is not None:
                self.assertIsNotNone(pr.license_allowed(expression), component)

    def test_lgpl_library_refused(self):
        self.add("lib/libmpfr.so.6")
        with self.assertRaisesRegex(RuntimeError, "no license rule.*libmpfr"):
            self.audit()

    def test_unknown_file_refused(self):
        # SeaHorn's test corpus has Linux-derived (GPL-2.0) inputs
        self.seahorn()
        self.add("share/seahorn/test/simple/ldv_bounce_sat.c")
        with self.assertRaisesRegex(RuntimeError, "no license rule.*ldv_bounce"):
            self.audit()

    def test_clam_refused(self):
        self.seahorn()
        self.add("include/clam/Clam.hh")
        with self.assertRaisesRegex(RuntimeError, "no license rule.*clam"):
            self.audit()

    def test_gpl_in_repository_refused(self):
        self.seahorn()
        self.licenses("seahorn", "GPL-2.0-only")
        with self.assertRaisesRegex(RuntimeError, "bin/sea.*GPL-2.0-only.*allowed"):
            self.audit()

    def test_missing_repository_licenses_refused(self):
        self.add("bin/sea")
        with self.assertRaisesRegex(RuntimeError, "repo:seahorn"):
            self.audit()

    def test_library_from_unknown_package_refused(self):
        libraries = {"libz.so.1": {"system": False, "package": "zlib1g:amd64"}}
        self.audit(libraries)
        libraries["libreadline.so.8"] = {
            "system": False,
            "package": "libreadline8t64:amd64",
        }
        with self.assertRaisesRegex(RuntimeError, "libreadline8t64"):
            self.audit(libraries)

    def test_apt_runtime_library_not_audited(self):
        libraries = {"libgmp.so.10": {"system": True, "apt": "libgmp10"}}
        self.audit(libraries)
