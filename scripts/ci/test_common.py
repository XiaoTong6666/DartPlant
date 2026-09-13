from __future__ import annotations

import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from common import (
    APK_BASE_NATIVE_ENTRIES,
    APK_DEFERRED_NATIVE_ENTRIES,
    APK_NATIVE_ENTRIES,
    ELF_MACHINE_AARCH64,
    inspect_arm64_apk,
)


def _elf64(machine: int = ELF_MACHINE_AARCH64) -> bytes:
    data = bytearray(20)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", data, 18, machine)
    return bytes(data)


def _write_apk(path: Path, entries: tuple[str, ...]) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        for entry in entries:
            archive.writestr(entry, _elf64())


class InspectArm64ApkTest(unittest.TestCase):
    def test_split_apks_are_inspected_as_one_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir) / "base.apk"
            deferred = Path(temp_dir) / "deferred.apk"
            _write_apk(base, APK_BASE_NATIVE_ENTRIES)
            _write_apk(deferred, APK_DEFERRED_NATIVE_ENTRIES)

            result = inspect_arm64_apk(base, deferred)

            self.assertEqual(result["native_entries"], list(APK_NATIVE_ENTRIES))
            self.assertEqual(result["deferred_apk"], str(deferred.resolve()))
            self.assertIn("deferred_sha256", result)

    def test_split_inspection_requires_deferred_image_in_feature(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir) / "base.apk"
            deferred = Path(temp_dir) / "deferred.apk"
            _write_apk(base, APK_BASE_NATIVE_ENTRIES)
            _write_apk(deferred, ())

            with self.assertRaisesRegex(ValueError, "libapp.so-2.part.so"):
                inspect_arm64_apk(base, deferred)


if __name__ == "__main__":
    unittest.main()
