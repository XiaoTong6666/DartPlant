#!/usr/bin/env python3
"""Check the exact-family test module before publishing its CI artifact."""

import argparse
import hashlib
import json
import subprocess
import tempfile
import zipfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--family", choices=("3.4.4", "3.5.0", "3.12.1"), required=True)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()

    required_entries = {
        "META-INF/xposed/module.prop",
        "META-INF/xposed/java_init.list",
        "META-INF/xposed/native_init.list",
        "META-INF/xposed/scope.list",
        "lib/arm64-v8a/libdartplant.so",
    }
    with zipfile.ZipFile(args.apk) as archive:
        names = set(archive.namelist())
        missing = required_entries - names
        if missing:
            raise RuntimeError(f"missing libxposed package entries: {sorted(missing)}")
        unexpected_abis = [
            path for path in names
            if path.startswith("lib/") and not path.startswith("lib/arm64-v8a/")
        ]
        if unexpected_abis:
            raise RuntimeError(f"unexpected module ABIs: {unexpected_abis}")
        prop = archive.read("META-INF/xposed/module.prop").decode()
        entries = archive.read("META-INF/xposed/native_init.list").decode().strip()
        scope = archive.read("META-INF/xposed/scope.list").decode().strip()
        java_entry = archive.read("META-INF/xposed/java_init.list").decode().strip()
        if not all(f"{key}=102" in prop for key in ("minApiVersion", "targetApiVersion")):
            raise RuntimeError("module must declare libxposed API 102")
        if "autoHotReload=false" not in prop:
            raise RuntimeError("native v2 module must initially refuse hot reload")
        if entries != "libdartplant.so" or scope != "dev.dartplant.dartplant_fixture":
            raise RuntimeError("module entry/scope differs from test contract")
        if java_entry != "dev.dartplant.integration.DartPlantModule":
            raise RuntimeError("unexpected Java module entry")
        native = archive.read("lib/arm64-v8a/libdartplant.so")
        if archive.getinfo("lib/arm64-v8a/libdartplant.so").compress_type != zipfile.ZIP_STORED:
            raise RuntimeError("Vector's APK classloader requires STORED native ELF")
    with tempfile.TemporaryDirectory() as directory:
        so = Path(directory) / "libdartplant.so"
        so.write_bytes(native)
        symbols = subprocess.check_output(
            [args.readelf, "-Ws", str(so)], text=True
        )
        dynamic_symbols = subprocess.check_output(
            [args.readelf, "--dyn-syms", "--wide", str(so)], text=True
        )
        dynamic = subprocess.check_output(
            [args.readelf, "-d", str(so)], text=True
        )
        fixed_family = subprocess.check_output(
            [args.readelf, "-p", ".dartplant.family", str(so)], text=True
        )
    for symbol in (
        "native_init",
        "dartplant_flutter_vm_adapter_create",
        "dartplant_module_external_bootstrap",
        "dartplant_module_external_counts",
        "dartplant_module_physical_mapping_control",
        "Java_dev_dartplant_integration_DartPlantModule_nativeStatus",
    ):
        if not any(line.rstrip().endswith(f" {symbol}") and " GLOBAL " in line
                   for line in symbols.splitlines()):
            raise RuntimeError(f"required exported symbol missing: {symbol}")
    for symbol in ("DobbyHook", "DobbyDestroy", "DobbyGetVersion", "DobbyCodePatch"):
        if any(
            line.rstrip().endswith(f" {symbol}") and " GLOBAL " in line
            for line in dynamic_symbols.splitlines()
        ):
            raise RuntimeError(
                f"vendored backend symbol must not interpose the LSPosed host: {symbol}"
            )
    if "NODELETE" not in dynamic:
        raise RuntimeError("native v2 has no callback unregister: NODELETE is mandatory")
    family_markers = [
        line.rsplit("]", 1)[-1].strip()
        for line in fixed_family.splitlines()
        if "]" in line
    ]
    if family_markers != [args.family]:
        raise RuntimeError(
            f"fixed adapter marker mismatch: expected {args.family}, got {family_markers}"
        )
    result = {
        "apk": args.apk.name,
        "family": args.family,
        "sha256": hashlib.sha256(args.apk.read_bytes()).hexdigest(),
        "native_sha256": hashlib.sha256(native).hexdigest(),
        "native_callback_resident": True,
        "scope": scope,
        "abi": "arm64-v8a",
    }
    if args.manifest:
        payload = json.loads(args.manifest.read_text())
        if payload["dart"] != args.family:
            raise RuntimeError("module family does not match Flutter artifact")
        payload["libxposed_module"] = result
        args.manifest.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
