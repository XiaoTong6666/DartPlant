#!/usr/bin/env python3

from __future__ import annotations

import subprocess as sp
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def configure(contents: str) -> str:
    with tempfile.TemporaryDirectory(prefix="dartplant-cmake-parent-") as temp:
        source = Path(temp)
        build = source / "build"
        (source / "CMakeLists.txt").write_text(contents)
        sp.run(
            ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"],
            check=True,
            stdout=sp.PIPE,
            stderr=sp.STDOUT,
            text=True,
        )
        return (build / "CMakeCache.txt").read_text()


def cache_value(cache: str, name: str) -> str | None:
    prefix = f"{name}:BOOL="
    for line in cache.splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :]
    return None


def main() -> int:
    cache = configure(
        f"""cmake_minimum_required(VERSION 3.22.1)
project(dartplant_parent_smoke LANGUAGES CXX)
set(CAPSTONE_BUILD_TESTS ON CACHE BOOL \"parent-owned\")
add_subdirectory(\"{ROOT}\" dartplant)
if(DARTPLANT_BUILD_TESTS)
  message(FATAL_ERROR \"DartPlant subdirectory default enabled tests\")
endif()
"""
    )
    assert cache_value(cache, "DARTPLANT_BUILD_TESTS") == "OFF"
    assert cache_value(cache, "CAPSTONE_BUILD_TESTS") == "ON"

    cache = configure(
        f"""cmake_minimum_required(VERSION 3.22.1)
project(dartplant_parent_capstone_smoke LANGUAGES C CXX)
set(DARTPLANT_BUILD_TESTS ON CACHE BOOL \"\")
set(CAPSTONE_BUILD_TESTS ON CACHE BOOL \"parent-owned\")
add_subdirectory(\"{ROOT}\" dartplant)
"""
    )
    assert cache_value(cache, "DARTPLANT_BUILD_TESTS") == "ON"
    assert cache_value(cache, "CAPSTONE_BUILD_TESTS") == "ON"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
