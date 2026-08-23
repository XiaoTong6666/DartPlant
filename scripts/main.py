from __future__ import annotations

import argparse
import json
from pathlib import Path

import audit as audit_cmd
import format as format_cmd
import lint as lint_cmd
import metadata as metadata_cmd
import flutter_cold_bootstrap as flutter_cold_cmd

from util import (
    BUILD_TYPE_TO_CMAKE,
    build_all,
    build_android,
    build_host,
    clean_outputs,
    create_context,
    doctor_snapshot,
    test_device,
    test_host,
)


def cmd_build(args: argparse.Namespace) -> None:
    ctx = create_context(args.build_type, ndk_override=args.ndk)
    if args.target == "host":
        build_host(ctx, force=args.force)
    elif args.target == "android":
        build_android(ctx, force=args.force)
    else:
        build_all(ctx, force=args.force)


def cmd_test(args: argparse.Namespace) -> None:
    if args.target == "flutter-cold":
        flutter_cold_cmd.run_flutter_cold_bootstrap_test(
            device=args.device,
            flutter=args.flutter,
            rounds=args.rounds,
            timeout_seconds=args.timeout,
            build=not args.no_flutter_build,
        )
        return

    ctx = create_context(args.build_type, ndk_override=args.ndk)
    if args.target in {"host", "all"}:
        test_host(ctx, force=args.force)
    if args.target in {"device", "all"}:
        test_device(ctx, device=args.device, force=args.force)


def cmd_doctor(args: argparse.Namespace) -> None:
    for key, value in doctor_snapshot(ndk_override=args.ndk).items():
        print(f"{key}: {value}")


def cmd_metadata(args: argparse.Namespace) -> None:
    try:
        result = metadata_cmd.generate_metadata(
            input_path=Path(args.input).resolve() if args.input else None,
            libapp_path=Path(args.libapp).resolve() if args.libapp else None,
            libflutter_path=Path(args.libflutter).resolve()
            if args.libflutter
            else None,
            output_path=Path(args.out).resolve(),
        )
    except metadata_cmd.MetadataError as error:
        raise SystemExit(f"metadata generation failed: {error}") from error
    print(json.dumps(result, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(prog="build.py")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build_parser = subparsers.add_parser("build")
    build_parser.add_argument("target", choices=["host", "android", "all"])
    build_parser.add_argument("-t", "--build-type", choices=BUILD_TYPE_TO_CMAKE, default="debug")
    build_parser.add_argument("--force", action="store_true")
    build_parser.add_argument("--ndk")
    build_parser.set_defaults(func=cmd_build)

    test_parser = subparsers.add_parser("test")
    test_parser.add_argument("target", choices=["host", "device", "flutter-cold", "all"])
    test_parser.add_argument("-t", "--build-type", choices=BUILD_TYPE_TO_CMAKE, default="debug")
    test_parser.add_argument("-s", "--device")
    test_parser.add_argument("--force", action="store_true")
    test_parser.add_argument("--ndk")
    test_parser.add_argument("--flutter")
    test_parser.add_argument("--rounds", type=int, default=10)
    test_parser.add_argument("--timeout", type=float, default=5.0)
    test_parser.add_argument("--no-flutter-build", action="store_true")
    test_parser.set_defaults(func=cmd_test)

    clean_parser = subparsers.add_parser("clean")
    clean_parser.set_defaults(func=lambda _: clean_outputs())

    doctor_parser = subparsers.add_parser("doctor")
    doctor_parser.add_argument("--ndk")
    doctor_parser.set_defaults(func=cmd_doctor)

    format_parser = subparsers.add_parser("format")
    format_parser.add_argument("--check", action="store_true")
    format_parser.add_argument("--list", action="store_true")
    format_parser.add_argument("--ndk")
    format_parser.set_defaults(
        func=lambda args: format_cmd.run_format(
            check=args.check, list_only=args.list, ndk=args.ndk
        )
    )

    lint_parser = subparsers.add_parser("lint")
    lint_parser.add_argument("-t", "--build-type", choices=BUILD_TYPE_TO_CMAKE, default="debug")
    lint_parser.add_argument("--ndk")
    lint_parser.set_defaults(
        func=lambda args: lint_cmd.run_lint(
            build_type=args.build_type, ndk=args.ndk
        )
    )

    audit_parser = subparsers.add_parser("audit")
    audit_parser.set_defaults(func=lambda _: audit_cmd.run_audit())

    metadata_parser = subparsers.add_parser("metadata")
    metadata_parser.add_argument("input", nargs="?")
    metadata_parser.add_argument("-o", "--out", required=True)
    metadata_parser.add_argument("--libapp")
    metadata_parser.add_argument("--libflutter")
    metadata_parser.set_defaults(func=cmd_metadata)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
