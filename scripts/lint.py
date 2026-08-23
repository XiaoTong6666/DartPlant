from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

import format as format_cmd

from util import BUILD_TYPE_TO_CMAKE, build_host, create_context, run


def run_lint(*, build_type: str, ndk: str | None) -> None:
    format_cmd.run_format(check=True, list_only=False, ndk=ndk)
    context = create_context(build_type, ndk_override=ndk)
    build_host(context)
    clang_tidy = shutil.which("clang-tidy")
    if clang_tidy:
        source_root = Path(__file__).resolve().parent.parent
        compile_commands_path = context.host_build_dir / "compile_commands.json"
        with compile_commands_path.open(encoding="utf-8") as stream:
            compile_commands = json.load(stream)
        compiled_files = {
            Path(command["file"]).resolve()
            for command in compile_commands
            if isinstance(command, dict) and "file" in command
        }
        sources = [
            path
            for path in sorted(source_root.glob("src/**/*.cpp"))
            if path.resolve() in compiled_files
        ]
        if sources:
            run(
                [
                    clang_tidy,
                    "-p",
                    str(context.host_build_dir),
                    *[str(path) for path in sources],
                ],
                cwd=context.host_build_dir,
            )


def main() -> None:
    parser = argparse.ArgumentParser(prog="main.py lint")
    parser.add_argument("-t", "--build-type", choices=BUILD_TYPE_TO_CMAKE, default="debug")
    parser.add_argument("--ndk")
    args = parser.parse_args()
    run_lint(build_type=args.build_type, ndk=args.ndk)


if __name__ == "__main__":
    main()
