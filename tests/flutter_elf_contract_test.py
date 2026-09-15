#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NDK_BIN = Path(
    "/opt/android-sdk/ndk/30.0.15729638/toolchains/llvm/prebuilt/linux-x86_64/bin"
)
SNAPSHOT_SYMBOLS = (
    "_kDartIsolateSnapshotData",
    "_kDartIsolateSnapshotInstructions",
    "_kDartSnapshotBuildId",
)


def run(*args: str) -> str:
    result = subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return result.stdout


def tool(name: str) -> str:
    candidate = DEFAULT_NDK_BIN / name
    if candidate.is_file():
        return str(candidate)
    resolved = shutil.which(name)
    if resolved:
        return resolved
    raise RuntimeError(f"required LLVM tool is unavailable: {name}")


def parse_probe(probe: Path, elf: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in run(str(probe), str(elf)).splitlines():
        key, sep, value = line.partition("\t")
        if not sep:
            raise AssertionError(f"malformed probe output: {line!r}")
        values[key] = value
    return values


def parse_dynamic_symbols(readelf: str, elf: Path) -> dict[str, tuple[int, int, str, str]]:
    output = run(readelf, "-s", "--wide", str(elf))
    found: dict[str, tuple[int, int, str, str]] = {}
    pattern = re.compile(
        r"^\s*\d+:\s*([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+\S+\s+(\S+)\s*$"
    )
    # llvm-readelf prints .dynsym before .symtab. Keep the first matching entry,
    # which is the authoritative dynamic symbol consumed by DartPlant.
    for line in output.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        value, size, typ, bind, name = match.groups()
        if name in SNAPSHOT_SYMBOLS and name not in found:
            found[name] = (int(value, 16), int(size), typ, bind)
    return found


def parse_loads(readelf: str, elf: Path) -> list[tuple[int, int, str]]:
    output = run(readelf, "-l", "--wide", str(elf))
    loads: list[tuple[int, int, str]] = []
    pattern = re.compile(
        r"^\s*LOAD\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+"
        r"0x([0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+(.+?)\s+0x[0-9a-fA-F]+\s*$"
    )
    for line in output.splitlines():
        match = pattern.match(line)
        if match:
            loads.append((int(match.group(1), 16), int(match.group(2), 16), match.group(3).replace(" ", "")))
    return loads


def containing_flags(loads: list[tuple[int, int, str]], value: int, size: int) -> str:
    matches = [flags for va, file_size, flags in loads if value >= va and size <= file_size and value - va <= file_size - size]
    if len(matches) != 1:
        raise AssertionError(f"symbol range 0x{value:x}+0x{size:x} has {len(matches)} PT_LOAD owners")
    return matches[0]


def parse_build_id(readelf: str, elf: Path) -> str:
    output = run(readelf, "-n", str(elf))
    match = re.search(r"Build ID:\s*([0-9a-fA-F]+)", output)
    if not match:
        raise AssertionError(f"LLVM found no GNU Build ID in {elf}")
    return match.group(1).lower()


def hash_style(readelf: str, elf: Path) -> str:
    output = run(readelf, "-d", str(elf))
    has_sysv = "(HASH)" in output
    has_gnu = "(GNU_HASH)" in output
    return "both" if has_sysv and has_gnu else "sysv" if has_sysv else "gnu" if has_gnu else "none"


def assert_nm_exports(nm: str, elf: Path) -> None:
    output = run(nm, "-D", "--defined-only", str(elf))
    for symbol in SNAPSHOT_SYMBOLS:
        if not re.search(rf"\b{re.escape(symbol)}$", output, re.MULTILINE):
            raise AssertionError(f"llvm-nm did not expose {symbol} in {elf}")


def assert_objdump_dynamic(objdump: str, elf: Path, expected_style: str) -> None:
    output = run(objdump, "-p", str(elf))
    if expected_style in {"sysv", "both"} and "HASH" not in output:
        raise AssertionError(f"llvm-objdump did not report DT_HASH for {elf}")
    if expected_style in {"gnu", "both"} and "GNU_HASH" not in output:
        raise AssertionError(f"llvm-objdump did not report DT_GNU_HASH for {elf}")


def check_one(probe: Path, elf: Path, *, deferred: bool) -> dict[str, str]:
    readelf = tool("llvm-readelf")
    objdump = tool("llvm-objdump")
    nm = tool("llvm-nm")
    header = run(readelf, "-h", str(elf))
    assert "Class:                             ELF64" in header
    assert "Machine:                           AArch64" in header

    actual = parse_probe(probe, elf)
    if actual.get("class") != "ELF64" or actual.get("machine") != "AArch64":
        raise AssertionError(f"DartPlant probe disagrees with LLVM ELF identity for {elf}")
    llvm_hash_style = hash_style(readelf, elf)
    if actual.get("hash_style") != llvm_hash_style:
        raise AssertionError(f"hash style mismatch for {elf}: probe={actual.get('hash_style')} llvm={llvm_hash_style}")
    assert_objdump_dynamic(objdump, elf, llvm_hash_style)
    assert_nm_exports(nm, elf)

    symbols = parse_dynamic_symbols(readelf, elf)
    loads = parse_loads(readelf, elf)
    if int(actual["load_count"], 0) != len(loads):
        raise AssertionError(f"PT_LOAD count mismatch for {elf}")
    for symbol in SNAPSHOT_SYMBOLS:
        if symbol not in symbols:
            raise AssertionError(f"LLVM dynamic symbol table lacks {symbol} in {elf}")
        value, size, typ, bind = symbols[symbol]
        if int(actual[f"symbol.{symbol}.value"], 0) != value or int(actual[f"symbol.{symbol}.size"], 0) != size:
            raise AssertionError(f"symbol value/size mismatch for {symbol} in {elf}")
        if typ != "OBJECT" or bind != "GLOBAL":
            raise AssertionError(f"LLVM symbol contract mismatch for {symbol} in {elf}: {typ}/{bind}")
        flags = containing_flags(loads, value, size)
        if symbol.endswith("Instructions"):
            if "R" not in flags or "E" not in flags:
                raise AssertionError(f"instructions symbol is not file-backed R|X in {elf}: {flags}")
        elif "R" not in flags:
            raise AssertionError(f"data symbol is not file-backed readable in {elf}: {flags}")

    if actual.get("build_id", "").lower() != parse_build_id(readelf, elf):
        raise AssertionError(f"GNU Build ID mismatch for {elf}")
    if deferred and actual.get("deferred_program_hash") in {None, "none"}:
        raise AssertionError(f"deferred unit lacks Dart serialized program hash: {elf}")
    return actual


def check_generic_dynamic(probe: Path, elf: Path, symbol_name: str) -> None:
    readelf = tool("llvm-readelf")
    objdump = tool("llvm-objdump")
    output = run(str(probe), str(elf), "--symbol", symbol_name)
    actual: dict[str, str] = {}
    for line in output.splitlines():
        key, sep, value = line.partition("\t")
        if not sep:
            raise AssertionError(f"malformed generic probe output: {line!r}")
        actual[key] = value
    llvm_style = hash_style(readelf, elf)
    if actual.get("hash_style") != llvm_style:
        raise AssertionError(f"generic hash style mismatch for {elf}")
    assert_objdump_dynamic(objdump, elf, llvm_style)
    symbols = run(readelf, "-s", "--wide", str(elf))
    pattern = re.compile(
        rf"^\s*\d+:\s*([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+\S+\s+{re.escape(symbol_name)}\s*$",
        re.MULTILINE,
    )
    match = pattern.search(symbols)
    if not match:
        raise AssertionError(f"LLVM cannot find {symbol_name} in {elf}")
    value, size, typ, bind = match.groups()
    if int(actual["generic_symbol.value"], 0) != int(value, 16):
        raise AssertionError(f"GNU-hash symbol VA mismatch for {symbol_name}")
    if int(actual["generic_symbol.size"], 0) != int(size):
        raise AssertionError(f"GNU-hash symbol size mismatch for {symbol_name}")
    if typ != "FUNC" or bind != "GLOBAL":
        raise AssertionError(f"unexpected LLVM export contract {typ}/{bind} for {symbol_name}")
    if actual.get("build_id", "").lower() != parse_build_id(readelf, elf):
        raise AssertionError(f"GNU-hash corpus Build ID mismatch for {elf}")


def discover_corpus() -> tuple[Path, Path, Path | None] | None:
    roots = sorted((ROOT / "tests/flutter_fixture/.dart_tool/flutter_build").glob("*/arm64-v8a/app.so"))
    for root in reversed(roots):
        unit = root.with_name("app.so-2.part.so")
        if unit.is_file():
            libflutter_candidates = sorted(
                (ROOT / "tests/flutter_fixture/build/app/intermediates/merged_native_libs/release").glob(
                    "**/lib/arm64-v8a/libflutter.so"
                )
            )
            return root, unit, libflutter_candidates[-1] if libflutter_candidates else None
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--require-corpus", action="store_true")
    args = parser.parse_args()
    corpus = discover_corpus()
    if corpus is None:
        if args.require_corpus:
            raise SystemExit("real Flutter deferred ARM64 corpus is missing")
        print("SKIP: no real Flutter deferred ARM64 corpus")
        return 0
    root, unit, libflutter = corpus
    root_result = check_one(args.probe, root, deferred=False)
    unit_result = check_one(args.probe, unit, deferred=True)
    if root_result["snapshot_hash"] != unit_result["snapshot_hash"]:
        raise AssertionError("root/deferred snapshot version hashes disagree")
    root_va = int(root_result["symbol._kDartIsolateSnapshotInstructions.value"], 0)
    unit_va = int(unit_result["symbol._kDartIsolateSnapshotInstructions.value"], 0)
    if root_va == unit_va:
        raise AssertionError("real deferred image did not establish an independent instruction VA namespace")
    if libflutter is not None:
        flutter_style = hash_style(tool("llvm-readelf"), libflutter)
        if flutter_style not in {"gnu", "both"}:
            raise AssertionError(f"expected current libflutter to exercise GNU hash, got {flutter_style}")
        check_generic_dynamic(
            args.probe, libflutter, "InternalFlutterGpu_Context_InitializeDefault"
        )
    print(f"PASS: root={root}")
    print(f"PASS: deferred={unit} program_hash={unit_result['deferred_program_hash']}")
    if libflutter is not None:
        print(f"PASS: libflutter GNU-hash corpus={libflutter}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
