from __future__ import annotations

import argparse

from util import BUILD_TYPE_TO_CMAKE, build_all, build_android, build_host, create_context


def main() -> None:
    parser = argparse.ArgumentParser(prog="main.py build")
    parser.add_argument("target", choices=["host", "android", "all"])
    parser.add_argument("-t", "--build-type", choices=BUILD_TYPE_TO_CMAKE, default="debug")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--ndk")
    args = parser.parse_args()
    context = create_context(args.build_type, ndk_override=args.ndk)
    if args.target == "host":
        build_host(context, force=args.force)
    elif args.target == "android":
        build_android(context, force=args.force)
    else:
        build_all(context, force=args.force)


if __name__ == "__main__":
    main()
