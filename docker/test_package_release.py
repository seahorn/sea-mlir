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

    def test_yices_library_refused(self):
        (self.prefix / "lib" / "libyices.so.2.7").write_bytes(ELF)
        with self.assertRaisesRegex(RuntimeError, "Yices"):
            pr.check_forbidden(self.prefix)

    def test_yices_binary_refused(self):
        (self.prefix / "bin" / "yices-smt2").write_bytes(ELF)
        with self.assertRaisesRegex(RuntimeError, "Yices"):
            pr.check_forbidden(self.prefix)

    def test_yices_dependency_refused(self):
        deps = [("libyices.so.2", self.library("libyices.so.2"))]
        with (
            patch.object(pr, "dependencies", return_value=deps),
            self.assertRaisesRegex(RuntimeError, "Yices"),
        ):
            pr.bundle_libraries(self.prefix, self.prefix / "lib", {})

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
        _, libraries, packages, copied = self.bundle("libgmp.so.10", "libgmp10:amd64")
        self.assertIn("libgmp10:amd64", packages)
        pr.require_licenses(libraries, copied)

    def test_known_upstream_library_accepted(self):
        _, libraries, _, copied = self.bundle("libz3.so.5.3")
        pr.require_licenses(libraries, copied)
        self.assertTrue(libraries["libz3.so.5.3"]["license"].endswith("Z3-LICENSE.txt"))

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
