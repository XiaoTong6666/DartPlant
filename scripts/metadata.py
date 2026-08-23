from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from util import ROOT_DIR


class MetadataError(RuntimeError):
    pass


def _indexer_binary() -> Path:
    configured = os.getenv("DARTPLANT_INDEXER")
    if configured:
        return Path(configured).expanduser().resolve()
    return ROOT_DIR / "build" / "tools" / "release" / "dartplant-indexer"


def _build_indexer() -> Path:
    binary = _indexer_binary()
    if binary.is_file():
        return binary
    manifest = ROOT_DIR / "tools" / "dartplant-indexer" / "Cargo.toml"
    result = subprocess.run(
        ["cargo", "build", "--manifest-path", str(manifest), "--release"],
        cwd=ROOT_DIR,
        env={
            **os.environ,
            "CC": os.environ.get("CC", "gcc"),
            "CXX": os.environ.get("CXX", "g++"),
            "CARGO_TARGET_DIR": str(ROOT_DIR / "build" / "tools"),
        },
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise MetadataError(
            "failed to build vendored flutterdec-backed indexer\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    built = ROOT_DIR / "build" / "tools" / "release" / "dartplant-indexer"
    if not built.is_file():
        raise MetadataError(f"indexer build produced no binary: {built}")
    return built


def _analysis_environment() -> dict[str, str]:
    env = dict(os.environ)
    for variable, fallback in (("CC", "gcc"), ("CXX", "g++")):
        value = env.get(variable, "")
        tool = value.split()[0] if value else ""
        if not tool or shutil.which(tool) is None:
            env[variable] = fallback
    return env


def _prepare_capstone_environment() -> dict[str, str]:
    capstone_root = ROOT_DIR / "third_party" / "capstone"
    if not capstone_root.joinpath("CMakeLists.txt").is_file():
        raise MetadataError("third_party/capstone submodule is not initialized")
    install_root = ROOT_DIR / "build" / "analysis" / "capstone"
    pkgconfig = install_root / "lib" / "pkgconfig"
    if not pkgconfig.joinpath("capstone.pc").is_file():
        build_root = ROOT_DIR / "build" / "analysis" / "capstone-build"
        env = _analysis_environment()
        configure = subprocess.run(
            [
                "cmake",
                "-S",
                str(capstone_root),
                "-B",
                str(build_root),
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCAPSTONE_BUILD_TESTS=OFF",
                "-DCAPSTONE_BUILD_CSTOOL=OFF",
                f"-DCMAKE_INSTALL_PREFIX={install_root}",
            ],
            cwd=ROOT_DIR,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )
        if configure.returncode != 0:
            raise MetadataError(
                f"failed to configure vendored Capstone\nstdout:\n{configure.stdout}\nstderr:\n{configure.stderr}"
            )
        build = subprocess.run(
            ["cmake", "--build", str(build_root), "--", f"-j{os.cpu_count() or 4}"],
            cwd=ROOT_DIR,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )
        if build.returncode != 0:
            raise MetadataError(
                f"failed to build vendored Capstone\nstdout:\n{build.stdout}\nstderr:\n{build.stderr}"
            )
        install = subprocess.run(
            ["cmake", "--install", str(build_root)],
            cwd=ROOT_DIR,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )
        if install.returncode != 0:
            raise MetadataError(
                f"failed to install vendored Capstone\nstdout:\n{install.stdout}\nstderr:\n{install.stderr}"
            )
    env = _analysis_environment()
    env["PKG_CONFIG_PATH"] = str(pkgconfig)
    env["C_INCLUDE_PATH"] = str(install_root / "include" / "capstone")
    env["CPLUS_INCLUDE_PATH"] = str(install_root / "include" / "capstone")
    return env


def generate_metadata(
    *,
    input_path: Path | None,
    libapp_path: Path | None,
    libflutter_path: Path | None,
    output_path: Path,
) -> dict[str, object]:
    if input_path is None and libapp_path is None:
        raise MetadataError("either input_path or libapp_path is required")
    source = input_path or libapp_path
    assert source is not None
    binary = _build_indexer()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    command = [str(binary), str(source), str(output_path)]
    if libflutter_path is not None:
        command += ["--libflutter", str(libflutter_path)]
    result = subprocess.run(
        command,
        cwd=ROOT_DIR,
        env=_prepare_capstone_environment(),
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise MetadataError(
            f"dartplant-indexer failed with status {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    payload = json.loads(output_path.read_text(encoding="utf-8"))
    return {
        "output": str(output_path),
        "adapter_kind": payload.get("adapter_kind", "unknown"),
        "snapshot_hash": payload.get("snapshot_hash", "unknown"),
        "emitted_methods": len(payload.get("methods", [])),
        "input_functions": len(payload.get("methods", [])),
    }


def main() -> None:
    parser = argparse.ArgumentParser(prog="main.py metadata")
    parser.add_argument("input", nargs="?", help="APK or libapp.so path")
    parser.add_argument("-o", "--out", required=True)
    parser.add_argument("--libapp")
    parser.add_argument("--libflutter")
    args = parser.parse_args()
    try:
        print(
            json.dumps(
                generate_metadata(
                    input_path=Path(args.input).resolve() if args.input else None,
                    libapp_path=Path(args.libapp).resolve() if args.libapp else None,
                    libflutter_path=Path(args.libflutter).resolve()
                    if args.libflutter
                    else None,
                    output_path=Path(args.out).resolve(),
                ),
                indent=2,
            )
        )
    except MetadataError as error:
        raise SystemExit(f"metadata generation failed: {error}") from error


if __name__ == "__main__":
    main()
