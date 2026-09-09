#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(ROOT_DIR / "scripts"))

import flutter_cold_bootstrap  # noqa: E402

from ci.common import inspect_arm64_apk  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build one DartPlant Flutter release AOT fixture for CI"
    )
    parser.add_argument("--flutter", required=True)
    parser.add_argument("--expected-flutter", required=True)
    parser.add_argument("--expected-dart", required=True)
    parser.add_argument("--family", required=True)
    parser.add_argument("--mode", choices=["release", "profile"], default="release")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    toolchain = flutter_cold_bootstrap.build_flutter_fixture(
        flutter=args.flutter, build_mode=args.mode
    )
    if toolchain.flutter_version != args.expected_flutter:
        raise RuntimeError(
            f"Flutter version mismatch: expected {args.expected_flutter}, "
            f"got {toolchain.flutter_version}"
        )
    if toolchain.dart_version != args.expected_dart:
        raise RuntimeError(
            f"Dart version mismatch: expected {args.expected_dart}, got {toolchain.dart_version}"
        )

    apk = flutter_cold_bootstrap.flutter_fixture_apk_path(args.mode)
    provenance = inspect_arm64_apk(apk)
    output_dir = args.out.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    output_apk = output_dir / f"app-{args.mode}.apk"
    shutil.copy2(apk, output_apk)
    provenance["apk"] = output_apk.name
    manifest = {
        "family": args.family,
        "flutter": toolchain.flutter_version,
        "dart": toolchain.dart_version,
        "mode": args.mode,
        **provenance,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
