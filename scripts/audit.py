from __future__ import annotations

import argparse
import re
from pathlib import Path

from source_tree import iter_text_files
from util import CONFIG_PATH, ROOT_DIR, load_config


def run_audit() -> None:
    issues: list[str] = []
    config = load_config()
    if config.abi != "arm64-v8a":
        issues.append("project-config.json: abi must be arm64-v8a")
    dobby_root = ROOT_DIR / "third_party" / "dobby"
    if not dobby_root.joinpath("CMakeLists.txt").is_file():
        issues.append(f"Dobby submodule is missing: {dobby_root}")
    for required in (
        ROOT_DIR / "CMakeLists.txt",
        ROOT_DIR / "include/dartplant/hook.h",
        ROOT_DIR / "include/dartplant/runtime.h",
        ROOT_DIR / "include/dartplant/runtime_profile.h",
        ROOT_DIR / "include/dartplant/advanced/runtime_profile.h",
        ROOT_DIR / "include/dartplant/adapters/dobby.h",
        ROOT_DIR / "include/dartplant/adapters/shadowhook.h",
        ROOT_DIR / "adapters/lsposed/lsposed_module.cpp",
        ROOT_DIR / "adapters/dobby/dobby_host.cpp",
        ROOT_DIR / "adapters/shadowhook/shadowhook_host.cpp",
        ROOT_DIR / "src/runtime/dart_runtime_resolver.cpp",
        ROOT_DIR / "src/runtime/default_runtime.cpp",
        ROOT_DIR / "scripts/main.py",
        ROOT_DIR / "scripts/metadata.py",
        ROOT_DIR / "tools/dartplant-indexer/Cargo.toml",
        ROOT_DIR / "third_party/flutterdec/Cargo.toml",
        ROOT_DIR / "third_party/blutter/blutter.py",
        ROOT_DIR / "third_party/dobby/CMakeLists.txt",
    ):
        if not required.exists():
            issues.append(f"missing required path: {required.relative_to(ROOT_DIR)}")
    for path in iter_text_files():
        if path == Path(__file__):
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        if re.search(r"/(?:home|Users)/[^/\s]+/", text):
            issues.append(
                f"{path.relative_to(ROOT_DIR)}: local user path leaked"
            )
    if issues:
        for issue in issues:
            print(f"AUDIT: {issue}")
        raise RuntimeError(f"audit failed with {len(issues)} issue(s)")
    print(f"audit passed: {CONFIG_PATH.relative_to(ROOT_DIR)}")


def main() -> None:
    argparse.ArgumentParser(prog="main.py audit").parse_args()
    run_audit()


if __name__ == "__main__":
    main()
