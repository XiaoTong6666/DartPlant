#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess as sp
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOBBY_ROOT = ROOT / "third_party" / "dobby"


def run(command: list[str]) -> None:
    result = sp.run(command, stdout=sp.PIPE, stderr=sp.STDOUT, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n{result.stdout}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", required=True)
    args = parser.parse_args()

    ndk = Path(args.ndk).resolve()
    toolchain = ndk / "build" / "cmake" / "android.toolchain.cmake"
    if not toolchain.is_file():
        raise SystemExit(f"Android NDK toolchain not found: {toolchain}")

    build_root = ROOT / "build"
    build_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dobby-arm64-nonear-", dir=build_root) as tmp:
        build = Path(tmp)
        run(
            [
                "cmake",
                "-S",
                str(DOBBY_ROOT),
                "-B",
                str(build),
                "-G",
                "Ninja",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                "-DANDROID_ABI=arm64-v8a",
                "-DANDROID_PLATFORM=android-24",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DNearBranch=OFF",
                "-DBUILD_TEST=OFF",
            ]
        )
        run(["cmake", "--build", str(build), "--target", "dobby", "--", "-j2"])

    print("[PASS] ARM64 Dobby builds with NearBranch=OFF")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
