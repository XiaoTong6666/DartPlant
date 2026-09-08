from __future__ import annotations

import hashlib
import struct
import zipfile
from pathlib import Path


PACKAGE = "dev.dartplant.dartplant_fixture"
ACTIVITY = f"{PACKAGE}/.MainActivity"
ARM64_ABI = "arm64-v8a"
ELF_MACHINE_AARCH64 = 183
RUNTIME_SCENARIOS = (
    "normal",
    "arguments_descriptor",
    "closure",
    "generic_closure",
    "generic_gc",
    "exception",
    "transition",
    "artifact_revalidate",
)
APK_NATIVE_ENTRIES = (
    "lib/arm64-v8a/libapp.so",
    "lib/arm64-v8a/libflutter.so",
    "lib/arm64-v8a/libdartplant_fixture_bridge.so",
)


def _elf_machine(data: bytes, *, entry: str) -> int:
    if len(data) < 20 or data[:4] != b"\x7fELF":
        raise ValueError(f"{entry} is not an ELF file")
    if data[4] != 2:
        raise ValueError(f"{entry} is not ELF64")
    byte_order = "<" if data[5] == 1 else ">" if data[5] == 2 else None
    if byte_order is None:
        raise ValueError(f"{entry} has an unsupported ELF byte order")
    return struct.unpack_from(f"{byte_order}H", data, 18)[0]


def inspect_arm64_apk(apk: Path) -> dict[str, object]:
    apk = apk.resolve()
    if not apk.is_file():
        raise FileNotFoundError(apk)
    digest = hashlib.sha256(apk.read_bytes()).hexdigest()
    with zipfile.ZipFile(apk) as archive:
        names = set(archive.namelist())
        missing = [entry for entry in APK_NATIVE_ENTRIES if entry not in names]
        if missing:
            raise ValueError(f"APK is missing ARM64 native entries: {', '.join(missing)}")
        machines = {
            entry: _elf_machine(archive.read(entry), entry=entry)
            for entry in APK_NATIVE_ENTRIES
        }
        non_arm64 = {
            entry: machine
            for entry, machine in machines.items()
            if machine != ELF_MACHINE_AARCH64
        }
        if non_arm64:
            raise ValueError(f"APK contains non-AArch64 native entries: {non_arm64}")
        forbidden_metadata = [
            name
            for name in names
            if "dartplant" in name.lower() and "metadata" in name.lower()
        ]
        if forbidden_metadata:
            raise ValueError(
                "APK unexpectedly packages DartPlant runtime metadata: "
                + ", ".join(sorted(forbidden_metadata))
            )
    return {
        "apk": str(apk),
        "sha256": digest,
        "abi": ARM64_ABI,
        "elf_machine": "AArch64",
        "native_entries": list(APK_NATIVE_ENTRIES),
    }
