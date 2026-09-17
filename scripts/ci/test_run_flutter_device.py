from __future__ import annotations

import tempfile
import unittest
import sys
from pathlib import Path
from unittest.mock import patch

import run_flutter_device
from common import PACKAGE

TEST_SERIAL = "device-under-test"
TEST_NETWORK_SERIAL = "network-device:5555"
TEST_EMULATOR_SERIAL = "emulator-1234"


class CleanInstallTest(unittest.TestCase):
    def test_clean_install_uninstalls_before_installing(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            apk = Path(temp_dir) / "fixture.apk"
            apk.write_bytes(b"apk")
            calls: list[list[str]] = []

            def fake_capture(
                command: list[str], *, check: bool = True, timeout: float = 30.0
            ) -> str:
                calls.append(command)
                if command[-3:] == ["pm", "path", PACKAGE]:
                    return ""
                if command[-2:] == ["pidof", PACKAGE]:
                    return ""
                if command[3:4] == ["uninstall"]:
                    return "Success\n"
                return "Success\n"

            with patch.object(run_flutter_device, "_capture", side_effect=fake_capture):
                uninstall = run_flutter_device._clean_install(
                    TEST_SERIAL, [apk], timeout=600.0
                )

            self.assertEqual(uninstall, "Success")
            self.assertEqual(
                calls[0],
                [
                    "adb",
                    "-s",
                    TEST_SERIAL,
                    "shell",
                    "am",
                    "force-stop",
                    PACKAGE,
                ],
            )
            self.assertIn(
                ["adb", "-s", TEST_SERIAL, "uninstall", PACKAGE],
                calls,
            )
            self.assertIn(
                ["adb", "-s", TEST_SERIAL, "shell", "pm", "path", PACKAGE],
                calls,
            )
            self.assertEqual(
                calls[-1],
                ["adb", "-s", TEST_SERIAL, "install", str(apk.resolve())],
            )

    def test_clean_install_rejects_stale_package(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            apk = Path(temp_dir) / "fixture.apk"
            apk.write_bytes(b"apk")

            def fake_capture(
                command: list[str], *, check: bool = True, timeout: float = 30.0
            ) -> str:
                if command[-3:] == ["pm", "path", PACKAGE]:
                    return "package:/data/app/base.apk\n"
                if command[-2:] == ["pidof", PACKAGE]:
                    return ""
                return "Success\n"

            with patch.object(run_flutter_device, "_capture", side_effect=fake_capture):
                with self.assertRaisesRegex(RuntimeError, "clean package state"):
                    run_flutter_device._clean_install(
                        TEST_SERIAL, [apk], timeout=0.01
                    )

    def test_clean_install_installs_base_and_deferred_split(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir) / "base.apk"
            deferred = Path(temp_dir) / "deferred.apk"
            base.write_bytes(b"base")
            deferred.write_bytes(b"deferred")
            calls: list[list[str]] = []

            def fake_capture(
                command: list[str], *, check: bool = True, timeout: float = 30.0
            ) -> str:
                calls.append(command)
                if command[-3:] == ["pm", "path", PACKAGE]:
                    return ""
                if command[-2:] == ["pidof", PACKAGE]:
                    return ""
                return "Success\n"

            with patch.object(run_flutter_device, "_capture", side_effect=fake_capture):
                run_flutter_device._clean_install(
                    TEST_SERIAL, [base, deferred], timeout=600.0
                )

            self.assertEqual(
                calls[-1],
                [
                    "adb",
                    "-s",
                    TEST_SERIAL,
                    "install-multiple",
                    "-r",
                    str(base.resolve()),
                    str(deferred.resolve()),
                ],
            )

    def test_clean_install_retries_vendor_delete_failure_after_force_stop(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            apk = Path(temp_dir) / "fixture.apk"
            apk.write_bytes(b"apk")
            calls: list[list[str]] = []
            uninstall_count = 0

            def fake_capture(
                command: list[str], *, check: bool = True, timeout: float = 30.0
            ) -> str:
                nonlocal uninstall_count
                calls.append(command)
                if command[-2:] == ["pidof", PACKAGE]:
                    return ""
                if command[3:4] == ["uninstall"]:
                    uninstall_count += 1
                    if uninstall_count == 1:
                        return "Failure [DELETE_FAILED_INTERNAL_ERROR]\n"
                    return "Success\n"
                if command[-3:] == ["pm", "path", PACKAGE]:
                    if uninstall_count == 1:
                        return "package:/data/app/base.apk\n"
                    return ""
                return "Success\n"

            with patch.object(run_flutter_device, "_capture", side_effect=fake_capture):
                with patch.object(run_flutter_device.time, "sleep", return_value=None):
                    uninstall = run_flutter_device._clean_install(
                        TEST_SERIAL, [apk], timeout=0.01
                    )

            self.assertEqual(
                uninstall, "Failure [DELETE_FAILED_INTERNAL_ERROR] | Success"
            )
            self.assertEqual(uninstall_count, 2)
            self.assertEqual(
                calls[0],
                [
                    "adb",
                    "-s",
                    TEST_SERIAL,
                    "shell",
                    "am",
                    "force-stop",
                    PACKAGE,
                ],
            )
            self.assertEqual(
                calls[-1],
                ["adb", "-s", TEST_SERIAL, "install", str(apk.resolve())],
            )


class RunnerTransportTest(unittest.TestCase):
    def test_launch_command_forces_fullscreen_for_deterministic_cold_start(self) -> None:
        self.assertEqual(
            run_flutter_device._launch_command(TEST_SERIAL, "all"),
            [
                "adb",
                "-s",
                TEST_SERIAL,
                "shell",
                "am",
                "start",
                "-W",
                "--windowingMode",
                "1",
                "-n",
                run_flutter_device.ACTIVITY,
                "--es",
                "dartplant_test",
                "all",
            ],
        )

    def test_resolve_serial_prefers_unique_emulator_over_network_device(self) -> None:
        devices = (
            "List of devices attached\n"
            f"{TEST_NETWORK_SERIAL}\tdevice\n"
            f"{TEST_EMULATOR_SERIAL}\tdevice\n"
        )
        with patch.object(run_flutter_device, "_capture", return_value=devices):
            self.assertEqual(
                run_flutter_device._resolve_serial(None), TEST_EMULATOR_SERIAL
            )

    def test_capture_replaces_invalid_utf8_from_logcat(self) -> None:
        output = run_flutter_device._capture(
            [
                sys.executable,
                "-c",
                "import sys; sys.stdout.buffer.write(b'ok\\n\\xffbad\\n')",
            ]
        )
        self.assertEqual(output, "ok\n\ufffdbad\n")


if __name__ == "__main__":
    unittest.main()
