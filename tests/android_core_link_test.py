#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess as sp
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str], *, cwd: Path | None = None) -> None:
    result = sp.run(command, cwd=cwd, stdout=sp.PIPE, stderr=sp.STDOUT, text=True)
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
    with tempfile.TemporaryDirectory(prefix="android-core-link-", dir=build_root) as tmp:
        source = Path(tmp)
        build = source / "build"
        (source / "consumer.cpp").write_text(
            """#include <cstdint>

extern "C" void dartplant_arm64_dispatch_exception_unwind(uintptr_t, uintptr_t);

extern "C" void dartplant_android_link_probe() {
  dartplant_arm64_dispatch_exception_unwind(1, 1);
}
"""
        )
        (source / "CMakeLists.txt").write_text(
            f"""cmake_minimum_required(VERSION 3.22.1)
project(dartplant_android_link_consumer LANGUAGES C CXX)
set(DARTPLANT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("{ROOT}" dartplant)
target_compile_definitions(dartplant_core PRIVATE DARTPLANT_EXCEPTION_UNWIND_LOGGING=1)
add_library(consumer SHARED consumer.cpp)
target_link_libraries(consumer PRIVATE dartplant_core)
"""
        )
        run(
            [
                "cmake",
                "-S",
                str(source),
                "-B",
                str(build),
                "-G",
                "Ninja",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                "-DANDROID_ABI=arm64-v8a",
                "-DANDROID_PLATFORM=android-24",
                "-DCMAKE_BUILD_TYPE=Release",
            ]
        )
        run(["cmake", "--build", str(build), "--target", "consumer", "--", "-j2"])

    print("[PASS] Android parent consumer links dartplant_core with liblog")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
