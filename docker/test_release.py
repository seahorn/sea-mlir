# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Release scheduling and cancellation tests; no Docker daemon is required."""

import importlib.util
import signal
import subprocess
import tempfile
import unittest
from datetime import UTC, datetime, time as clock
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
                self.assertEqual(release.cutoff(self.local(start), *NIGHT), self.local(end))
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


if __name__ == "__main__":
    unittest.main()
