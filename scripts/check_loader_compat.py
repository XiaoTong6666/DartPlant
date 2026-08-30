from __future__ import annotations

import argparse
import shutil
import subprocess as sp
from pathlib import Path


FORBIDDEN_AARCH64_RELOCATIONS = {
    "R_AARCH64_TLSDESC",
    "R_AARCH64_TLS_DTPMOD",
    "R_AARCH64_TLS_DTPREL",
    "R_AARCH64_TLS_TPREL",
}


def forbidden_relocations(readelf_output: str) -> set[str]:
    found: set[str] = set()
    for line in readelf_output.splitlines():
        for relocation in FORBIDDEN_AARCH64_RELOCATIONS:
            if relocation in line:
                found.add(relocation)
    return found


def check_binary(binary: Path, *, readelf: str | None = None) -> None:
    tool = readelf or shutil.which("llvm-readelf") or shutil.which("readelf")
    if tool is None:
        raise RuntimeError("llvm-readelf/readelf not found")
    result = sp.run(
        [tool, "-rW", str(binary)],
        check=False,
        stdout=sp.PIPE,
        stderr=sp.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"failed to inspect ELF relocations:\n{result.stdout}")
    forbidden = forbidden_relocations(result.stdout)
    if forbidden:
        joined = ", ".join(sorted(forbidden))
        raise RuntimeError(
            f"{binary}: not compatible with a minimal custom loader; "
            f"dynamic TLS relocations present: {joined}. Use the system linker "
            "or a pthread-key TLS backend before enabling this contract."
        )
    print(f"minimal-loader compatibility passed: {binary}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--readelf")
    args = parser.parse_args()
    check_binary(args.binary, readelf=args.readelf)


if __name__ == "__main__":
    main()
