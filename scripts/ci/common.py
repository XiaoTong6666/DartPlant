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
    "deferred_lifecycle",
    "multi_engine",
)
COMMON_RUNTIME_SCENARIOS = (
    "initialization",
    "local_gate",
    "simple_facade",
    "p6_abi",
    "exception_bridge",
    "closure_receiver",
    "advanced_ordinary",
    "null_semantics",
    "bool_semantics",
    "live_vm_startup",
    "ordinary_aot",
    "late_shared",
)
APK_NATIVE_ENTRIES = (
    "lib/arm64-v8a/libapp.so",
    "lib/arm64-v8a/libapp.so-2.part.so",
    "lib/arm64-v8a/libflutter.so",
    "lib/arm64-v8a/libdartplant_fixture_bridge.so",
)
APK_BASE_NATIVE_ENTRIES = tuple(
    entry for entry in APK_NATIVE_ENTRIES if entry != "lib/arm64-v8a/libapp.so-2.part.so"
)
APK_DEFERRED_NATIVE_ENTRIES = ("lib/arm64-v8a/libapp.so-2.part.so",)


def _elf_machine(data: bytes, *, entry: str) -> int:
    if len(data) < 20 or data[:4] != b"\x7fELF":
        raise ValueError(f"{entry} is not an ELF file")
    if data[4] != 2:
        raise ValueError(f"{entry} is not ELF64")
    byte_order = "<" if data[5] == 1 else ">" if data[5] == 2 else None
    if byte_order is None:
        raise ValueError(f"{entry} has an unsupported ELF byte order")
    return struct.unpack_from(f"{byte_order}H", data, 18)[0]


def inspect_arm64_apk(
    apk: Path, deferred_apk: Path | None = None
) -> dict[str, object]:
    apk = apk.resolve()
    if not apk.is_file():
        raise FileNotFoundError(apk)
    digest = hashlib.sha256(apk.read_bytes()).hexdigest()
    apk_entries = (
        APK_BASE_NATIVE_ENTRIES if deferred_apk is not None else APK_NATIVE_ENTRIES
    )
    archives = [(apk, apk_entries)]
    if deferred_apk is not None:
        deferred_apk = deferred_apk.resolve()
        if not deferred_apk.is_file():
            raise FileNotFoundError(deferred_apk)
        archives.append((deferred_apk, APK_DEFERRED_NATIVE_ENTRIES))

    machines: dict[str, int] = {}
    for archive_path, required_entries in archives:
        with zipfile.ZipFile(archive_path) as archive:
            names = set(archive.namelist())
            missing = [entry for entry in required_entries if entry not in names]
            if missing:
                raise ValueError(
                    f"APK is missing ARM64 native entries: {', '.join(missing)}"
                )
            machines.update(
                {
                    entry: _elf_machine(archive.read(entry), entry=entry)
                    for entry in required_entries
                }
            )
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

    non_arm64 = {
        entry: machine
        for entry, machine in machines.items()
        if machine != ELF_MACHINE_AARCH64
    }
    if non_arm64:
        raise ValueError(f"APK contains non-AArch64 native entries: {non_arm64}")

    result: dict[str, object] = {
        "apk": str(apk),
        "sha256": digest,
        "abi": ARM64_ABI,
        "elf_machine": "AArch64",
        "native_entries": list(APK_NATIVE_ENTRIES),
    }
    if deferred_apk is not None:
        result["deferred_apk"] = str(deferred_apk)
        result["deferred_sha256"] = hashlib.sha256(deferred_apk.read_bytes()).hexdigest()
    return result
