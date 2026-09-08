from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import run_flutter_device
from common import PACKAGE


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
                if command[3:4] == ["uninstall"]:
                    return "Success\n"
                return "Success\n"

            with patch.object(run_flutter_device, "_capture", side_effect=fake_capture):
                uninstall = run_flutter_device._clean_install(
                    "emulator-5554", apk, timeout=600.0
                )

            self.assertEqual(uninstall, "Success")
            self.assertEqual(calls[0], ["adb", "-s", "emulator-5554", "uninstall", PACKAGE])
            self.assertEqual(
                calls[1],
                ["adb", "-s", "emulator-5554", "shell", "pm", "path", PACKAGE],
            )
            self.assertEqual(
                calls[2],
                ["adb", "-s", "emulator-5554", "install", str(apk.resolve())],
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
                return "Success\n"

            with patch.object(run_flutter_device, "_capture", side_effect=fake_capture):
                with self.assertRaisesRegex(RuntimeError, "clean package state"):
                    run_flutter_device._clean_install(
                        "emulator-5554", apk, timeout=600.0
                    )


if __name__ == "__main__":
    unittest.main()
