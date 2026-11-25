# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Release scheduling and cancellation tests; no Docker daemon is required."""

import importlib.util
import re
import signal
import subprocess
import tempfile
import unittest
from datetime import UTC, datetime
from datetime import time as clock
from pathlib import Path
from unittest.mock import Mock, patch
from zoneinfo import ZoneInfo

spec = importlib.util.spec_from_file_location(
    "release_driver", Path(__file__).with_name("release.py")
)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def zone_or_none(name):
    try:
        return ZoneInfo(name)
    except Exception:  # no tz database on this machine
        return None


NIGHT = (clock(7, 0), clock(22, 0))  # until, not_before


class CutoffTests(unittest.TestCase):
    toronto = zone_or_none("America/Toronto")

    def setUp(self):
        if self.toronto is None:
            self.skipTest("tz database unavailable")

    def local(self, value):
        return datetime.fromisoformat(value).replace(tzinfo=self.toronto)

    def test_window_boundaries(self):
        for start, end in (
            ("2026-11-06T22:00:00", "2026-11-07T07:00:00"),
            ("2026-11-06T23:59:59", "2026-11-07T07:00:00"),
            ("2026-11-07T00:00:00", "2026-11-07T07:00:00"),
            ("2026-11-07T06:59:59.999999", "2026-11-07T07:00:00"),
        ):
            with self.subTest(start=start):
                self.assertEqual(
                    release.cutoff(self.local(start), *NIGHT), self.local(end)
                )
        for start in ("2026-11-07T07:00:00", "2026-11-07T21:59:59.999999"):
            with self.subTest(start=start), self.assertRaises(ValueError):
                release.cutoff(self.local(start), *NIGHT)

    def test_dst_changes_elapsed_window_but_not_local_cutoff(self):
        for start, utc_end, hours in (
            ("2026-03-07T22:00:00", "2026-03-08T11:00:00+00:00", 8),
            ("2026-10-31T22:00:00", "2026-11-01T12:00:00+00:00", 10),
        ):
            with self.subTest(start=start):
                now = self.local(start)
                end = release.cutoff(now, *NIGHT)
                self.assertEqual(end.hour, 7)
                self.assertEqual(end.astimezone(UTC), datetime.fromisoformat(utc_end))
                self.assertEqual(end.timestamp() - now.timestamp(), hours * 3600)

    def test_repeated_fall_hour_has_one_deadline(self):
        first = self.local("2026-11-01T01:30:00")
        second = first.replace(fold=1)
        end = lambda now: release.cutoff(now, *NIGHT).timestamp()
        self.assertEqual(end(first), end(second))
        self.assertEqual(end(first) - first.timestamp(), 6.5 * 3600)
        self.assertEqual(end(second) - second.timestamp(), 5.5 * 3600)

    def test_until_alone_has_no_start_restriction(self):
        until = clock(7, 0)
        self.assertEqual(
            release.cutoff(self.local("2026-11-06T12:00:00"), until),
            self.local("2026-11-07T07:00:00"),
        )
        self.assertEqual(
            release.cutoff(self.local("2026-11-06T07:00:00"), until),
            self.local("2026-11-07T07:00:00"),
        )
        self.assertEqual(
            release.cutoff(self.local("2026-11-06T06:00:00"), until),
            self.local("2026-11-06T07:00:00"),
        )

    def test_same_day_window(self):
        end, start = clock(17, 30), clock(9, 0)
        self.assertEqual(
            release.cutoff(self.local("2026-11-06T10:00:00"), end, start),
            self.local("2026-11-06T17:30:00"),
        )
        with self.assertRaises(ValueError):
            release.cutoff(self.local("2026-11-06T08:59:00"), end, start)


class DescribeTests(unittest.TestCase):
    def repo(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name)

        def git(*args):
            subprocess.run(
                [
                    "git",
                    "-C",
                    tmp.name,
                    "-c",
                    "user.name=t",
                    "-c",
                    "user.email=t@t",
                    *args,
                ],
                check=True,
                capture_output=True,
            )

        git("init", "-q")
        (self.path / "f").write_text("a\n")
        git("add", "f")
        git("commit", "-q", "-m", "a")
        return git

    def test_tag(self):
        git = self.repo()
        git("tag", "v1.2.3")
        self.assertEqual(release.source_describe(self.path), "v1.2.3")

    def test_no_tag_falls_back(self):
        self.repo()
        sha = subprocess.check_output(
            ["git", "-C", str(self.path), "rev-parse", "HEAD"], text=True
        ).strip()
        self.assertEqual(
            release.source_describe(self.path), sha[:12] + "-release-build"
        )

    def test_dirty(self):
        git = self.repo()
        git("tag", "v1.2.3")
        (self.path / "f").write_text("b\n")
        self.assertEqual(release.source_describe(self.path), "v1.2.3-dirty")
        git("tag", "-d", "v1.2.3")
        self.assertTrue(
            release.source_describe(self.path).endswith("-release-build-dirty")
        )

    def test_after_tag(self):
        git = self.repo()
        git("tag", "v1.2.3")
        (self.path / "f").write_text("b\n")
        git("commit", "-q", "-am", "b")
        self.assertRegex(
            release.source_describe(self.path), r"^v1\.2\.3-1-g[0-9a-f]{12}$"
        )


class SmokeCommandTests(unittest.TestCase):
    def job(self, described):
        job = Mock()
        job.record = {"source_describe": described}
        return job

    def test_tagged_source_passes_smoke_tag(self):
        job = self.job("v0.5.0")
        command = release.smoke_command(job, Path("out"), "0.5.0", "--bundle")
        self.assertEqual(command[:2], ["env", "SMOKE_TAG=v0.5.0"])
        self.assertEqual(command[-2:], ["0.5.0", "--bundle"])
        self.assertNotIn("smoke_tag_warning", job.record)

    def test_untagged_source_warns_and_skips(self):
        job = self.job("v0.5.0-1-gabcdef123456")
        with patch("builtins.print") as printed:
            command = release.smoke_command(job, Path("out"), "0.5.0")
        self.assertEqual(command[0], "bash")
        self.assertFalse(any("SMOKE_TAG" in part for part in command))
        self.assertIn("version-tag check skipped", job.record["smoke_tag_warning"])
        self.assertIn("v0.5.0-1-gabcdef123456", printed.call_args.args[0])
        job.save.assert_called()


class SystemZoneTests(unittest.TestCase):
    def test_naive_arithmetic_in_system_zone(self):
        now = datetime.fromisoformat("2026-06-01T12:00:00").astimezone()
        end = release.cutoff(now, clock(7, 0))
        self.assertEqual(end.replace(tzinfo=None).isoformat(), "2026-06-02T07:00:00")
        self.assertGreater(end, now)


class CancellationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.job = release.ReleaseJob.__new__(release.ReleaseJob)
        self.job.source = Path(temporary.name)
        self.job.output = self.job.source
        self.job.logs = self.job.source
        self.job.deadline = datetime.fromtimestamp(1000, UTC)
        self.job.record = {"stages": []}
        self.job.save = Mock()

    def test_expired_window_starts_no_process(self):
        with (
            patch.object(release.time, "time", return_value=1000),
            patch.object(release.subprocess, "Popen") as popen,
        ):
            with self.assertRaises(TimeoutError):
                self.job.run("late", ["docker", "build", "."])
        popen.assert_not_called()

    def test_timeout_escalates_and_cleans_only_logged_container(self):
        process = Mock(pid=12345)
        process.wait.side_effect = [
            subprocess.TimeoutExpired("docker", 100),
            subprocess.TimeoutExpired("docker", 20),
            -signal.SIGKILL,
        ]
        owned = "0123456789ab"

        def launch(command, **kwargs):
            kwargs["stdout"].write(
                "Step 1/2 : RUN build\n ---> Running in " + owned + "\n"
            )
            kwargs["stdout"].flush()
            self.assertTrue(kwargs["start_new_session"])
            return process

        with (
            patch.object(release.time, "time", return_value=900),
            patch.object(release.subprocess, "Popen", side_effect=launch),
            patch.object(release.os, "killpg") as killpg,
            patch.object(release.subprocess, "run") as cleanup,
        ):
            with self.assertRaises(TimeoutError):
                self.job.run("build", ["docker", "build", "."])
        self.assertEqual(
            [call.args for call in killpg.call_args_list],
            [(12345, signal.SIGINT), (12345, signal.SIGKILL)],
        )
        self.assertEqual(process.wait.call_args_list[0].kwargs["timeout"], 100)
        self.assertEqual(self.job.record["stages"][-1]["exit"], 124)
        self.assertTrue(cleanup.called)
        for call in cleanup.call_args_list:
            self.assertIn(owned, call.args[0])
            self.assertNotIn("prune", call.args[0])
        self.job.save.assert_called()

    def test_extraction_timeout_removes_its_container(self):
        owned = "0123456789abcdef"
        self.job.run = Mock(side_effect=TimeoutError("expired"))
        with (
            patch.object(release.subprocess, "check_output", return_value=owned + "\n"),
            patch.object(release.subprocess, "run") as cleanup,
        ):
            with self.assertRaises(TimeoutError):
                self.job.extract("sea-mlir-release:package")
        cleanup.assert_called_once()
        self.assertEqual(cleanup.call_args.args[0], ["docker", "rm", owned])

    def test_cleanup_failure_preserves_timeout_after_process_exits(self):
        process = Mock(pid=12345)
        process.wait.side_effect = [subprocess.TimeoutExpired("docker", 100), 0]
        owned = "sea-mlir-smoke-123-456"

        def launch(command, **kwargs):
            kwargs["stdout"].write("smoke-container: " + owned + "\n")
            kwargs["stdout"].flush()
            return process

        with (
            patch.object(release.time, "time", return_value=900),
            patch.object(release.subprocess, "Popen", side_effect=launch),
            patch.object(release.os, "killpg", side_effect=ProcessLookupError),
            patch.object(
                release.subprocess,
                "run",
                side_effect=subprocess.TimeoutExpired("docker stop", 15),
            ) as cleanup,
        ):
            with self.assertRaises(TimeoutError):
                self.job.run("smoke", ["bash", "docker/smoke/run.sh"])
        cleanup.assert_called_once()
        self.assertIn(owned, cleanup.call_args.args[0])
        self.assertEqual(self.job.record["stages"][-1]["exit"], 124)


class PinTests(unittest.TestCase):
    def test_solver_pins_from_dockerfile(self):
        # the packaging gets all three solver pins, from docker/Dockerfile
        found = release.pins(Path(__file__).resolve().parent.parent)
        self.assertEqual(
            [pin.split("=", 1)[0] for pin in found],
            ["Z3_REVISION", "BITWUZLA_TAG", "YICES_TAG"],
        )
        self.assertTrue(all(pin.split("=", 1)[1] for pin in found))


ROOT = Path(__file__).resolve().parent.parent
FORMULA = ROOT / "packaging" / "homebrew" / "Formula"


def docker_arg(path, name):
    found = re.search(rf"^ARG {name}=(\S+)$", path.read_text(), re.MULTILINE)
    assert found, f"no ARG {name} in {path}"
    return found.group(1)


class HomebrewFormulaTests(unittest.TestCase):
    """The tap's formulae build the pins of the Docker release."""

    def test_sea_mlir_pins(self):
        text = (FORMULA / "sea-mlir.rb").read_text()
        dockerfile = ROOT / "docker" / "Dockerfile"
        z3 = docker_arg(dockerfile, "Z3_REVISION")
        self.assertIn(f"github.com/Z3Prover/z3/archive/{z3}.tar.gz", text)
        bitwuzla = docker_arg(dockerfile, "BITWUZLA_TAG")
        self.assertIn(f"bitwuzla/archive/refs/tags/{bitwuzla}.tar.gz", text)
        yices = docker_arg(dockerfile, "YICES_TAG")
        self.assertIn(f"yices2/archive/refs/tags/{yices}.tar.gz", text)

    def test_llvm_pin(self):
        text = (FORMULA / "sea-mlir-llvm.rb").read_text()
        commit = (ROOT / "LLVM_REVISION").read_text().split("commit:")[1].split()[0]
        self.assertIn(f"llvm-project/archive/{commit}.tar.gz", text)
        self.assertIn(f'version "24.0.0-git.{commit[:12]}"', text)

    def test_seahorn_pins(self):
        text = (FORMULA / "seahorn.rb").read_text()
        dockerfile = ROOT / "docker" / "SeaHorn.Dockerfile"
        for arg, repository in (
            ("SEAHORN_REVISION", "seahorn"),
            ("SEADSA_REVISION", "sea-dsa"),
            ("LLVM_SEAHORN_REVISION", "llvm-seahorn"),
        ):
            with self.subTest(arg=arg):
                commit = docker_arg(dockerfile, arg)
                self.assertIn(f"seahorn/{repository}/archive/{commit}.tar.gz", text)
        # the tag z3-4.12.0 is SEAHORN_Z3_REVISION (git ls-remote)
        self.assertEqual(
            docker_arg(dockerfile, "SEAHORN_Z3_REVISION"),
            "feda706d0dda2539176246dd221328122c66f77d",
        )
        self.assertIn("z3/archive/refs/tags/z3-4.12.0.tar.gz", text)

    def test_seahorn_patch_is_the_docker_patch(self):
        text = (FORMULA / "seahorn.rb").read_text()
        embedded = text.split("\n__END__\n", 1)[1]
        patch_text = (ROOT / "docker" / "seahorn-yices.patch").read_text()
        self.assertEqual(embedded, patch_text)

    def test_set_formula_source_replaces_top_level_only(self):
        text = (FORMULA / "sea-mlir.rb").read_text()
        local = release.set_formula_source(
            text,
            "file:///src/sea-mlir-0.5.0-local.abc.tar.gz",
            "f" * 64,
            "0.5.0-local.abc",
        )
        self.assertIn('  url "file:///src/sea-mlir-0.5.0-local.abc.tar.gz"\n', local)
        self.assertIn('  version "0.5.0-local.abc"\n', local)
        self.assertIn(f'  sha256 "{"f" * 64}"\n', local)
        self.assertNotIn("refs/tags/v", local)
        # the resources keep their url and sha256
        self.assertEqual(local.count("    url "), text.count("    url "))
        self.assertEqual(local.count("    sha256 "), text.count("    sha256 "))

    def test_local_formulae_are_not_publishable(self):
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory)
            (artifacts / "Formula").mkdir()
            (artifacts / "Formula" / "sea-mlir.rb").write_text("class SeaMlir\n")
            local = release.mark_local_formulae(
                artifacts, {"commit": "a" * 40, "version": "1.2.3-local.aaaaaaaaaaaa"}
            )
            self.assertEqual(local, artifacts / "Formula-local")
            self.assertFalse((artifacts / "Formula").exists())
            text = (local / "sea-mlir.rb").read_text()
            self.assertTrue(text.startswith("# NOT FOR PUBLISHING."))
            self.assertIn("1.2.3-local.aaaaaaaaaaaa", text)
            self.assertTrue(text.endswith("class SeaMlir\n"))

    def test_brew_source_version(self):
        with tempfile.TemporaryDirectory() as directory:
            git = ["git", "-C", directory, "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(["git", "init", "-q", directory], check=True)
            Path(directory, "CMakeLists.txt").write_text(
                "project(sea-mlir VERSION 1.2.3 LANGUAGES CXX)\n"
            )
            subprocess.run([*git, "add", "."], check=True)
            subprocess.run([*git, "commit", "-q", "-m", "a"], check=True)
            version, commit, tagged = release.brew_source(Path(directory), "HEAD")
            self.assertEqual(version, f"1.2.3-local.{commit[:12]}")
            self.assertFalse(tagged)
            subprocess.run([*git, "tag", "v1.2.3"], check=True)
            self.assertEqual(
                release.brew_source(Path(directory), "HEAD"), ("1.2.3", commit, True)
            )

    def homebrew_args(self, brew_source):
        return Mock(brew_source=brew_source, cpuset=None)

    def test_formula_source_dirty_tree_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            git = ["git", "-C", directory, "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(["git", "init", "-q", directory], check=True)
            Path(directory, "a").write_text("1\n")
            subprocess.run([*git, "add", "."], check=True)
            subprocess.run([*git, "commit", "-q", "-m", "a"], check=True)
            source = Path(directory)
            release.require_clean_source(source)
            Path(directory, "a").write_text("2\n")
            with self.assertRaisesRegex(RuntimeError, "clean source tree"):
                release.require_clean_source(source)
            job = Mock()
            job.record = {}
            with patch.object(release, "homebrew_context") as context:
                status = release.homebrew(
                    job, source, source.parent / "out", self.homebrew_args("formula")
                )
            self.assertEqual(status, 1)
            self.assertEqual(job.record["status"], "failed")
            context.assert_not_called()
            job.build.assert_not_called()

    def test_local_source_dirty_tree_not_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["git", "init", "-q", directory], check=True)
            Path(directory, "a").write_text("1\n")
            job = Mock()
            job.record = {}
            with (
                patch.object(release, "homebrew_context", side_effect=OSError("stop")),
                patch.object(release, "require_clean_source") as clean,
            ):
                release.homebrew(
                    job,
                    Path(directory),
                    Path(directory).parent / "out",
                    self.homebrew_args("HEAD"),
                )
            clean.assert_not_called()


if __name__ == "__main__":
    unittest.main()
