from __future__ import annotations

import argparse
import shutil
import subprocess as sp
from pathlib import Path

from util import ROOT_DIR, create_context, resolve_android_home
from source_tree import CPP_EXTENSIONS, RUST_EXTENSIONS, iter_files


def resolve_clang_format(ndk_override: str | None = None) -> str:
    try:
        ctx = create_context("debug", ndk_override=ndk_override)
        llvm_bin = ctx.ndk_home / "toolchains" / "llvm" / "prebuilt"
        prebuilt = next(path for path in llvm_bin.iterdir() if path.is_dir())
        candidate = prebuilt / "bin" / "clang-format"
        if candidate.exists():
            return str(candidate)
    except Exception:
        pass

    android_home = None
    try:
        android_home = resolve_android_home()
    except Exception:
        android_home = None
    if android_home is not None:
        candidate = next((p for p in android_home.glob("ndk/*/toolchains/llvm/prebuilt/*/bin/clang-format") if p.exists()), None)
        if candidate is not None:
            return str(candidate)

    clang_format = shutil.which("clang-format")
    if clang_format:
        return clang_format
    raise FileNotFoundError("clang-format not found")


def resolve_rustfmt() -> str:
    rustfmt = shutil.which("rustfmt")
    if rustfmt:
        return rustfmt
    raise FileNotFoundError("rustfmt not found")


def run(cmd: list[str]) -> None:
    result = sp.run(cmd, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit code {result.returncode}: {cmd}")


def format_cpp(files: list[Path], *, check: bool, ndk: str | None = None) -> None:
    if not files:
        return
    clang_format = resolve_clang_format(ndk_override=ndk)
    for path in files:
        cmd = [clang_format, "--style=file", str(path)]
        if check:
            cmd.insert(1, "--dry-run")
            cmd.insert(2, "--Werror")
        else:
            cmd.insert(1, "-i")
        run(cmd)


def format_rust(files: list[Path], *, check: bool) -> None:
    if not files:
        return
    rustfmt = resolve_rustfmt()
    cmd = [rustfmt, "--edition", "2024"]
    if check:
        cmd.append("--check")
    cmd.extend(str(path) for path in files)
    run(cmd)


def run_format(*, check: bool, list_only: bool, ndk: str | None) -> None:
    rust_files = iter_files(RUST_EXTENSIONS)
    cpp_files = iter_files(CPP_EXTENSIONS)

    if list_only:
        for path in rust_files + cpp_files:
            print(path.relative_to(ROOT_DIR).as_posix())
        return

    format_rust(rust_files, check=check)
    format_cpp(cpp_files, check=check, ndk=ndk)


def main() -> None:
    parser = argparse.ArgumentParser(prog="main.py format")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--ndk")
    args = parser.parse_args()
    run_format(check=args.check, list_only=args.list, ndk=args.ndk)


if __name__ == "__main__":
    main()
