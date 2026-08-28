#!/usr/bin/env python3

from __future__ import annotations

import argparse
import io
import json
import re
import subprocess as sp
import tarfile
import tempfile
from pathlib import Path
from urllib.parse import unquote, urljoin, urlparse


_DART_VERSION_RE = re.compile(r"Dart SDK version: (?P<version>\d+\.\d+\.\d+)")
_SDK_CONSTRAINT_RE = re.compile(
    r"^\s+sdk:\s*['\"]?[^0-9]*(?P<major>\d+)\.(?P<minor>\d+)", re.MULTILINE
)
_INTERNAL_PACKAGES = ("kernel", "vm", "_fe_analyzer_shared", "front_end")


def _run(command: list[str], *, cwd: Path | None = None) -> str:
    result = sp.run(
        command,
        cwd=cwd,
        check=False,
        stdout=sp.PIPE,
        stderr=sp.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed with exit code {result.returncode}: {command}\n{result.stdout}"
        )
    return result.stdout


def _dart_version(dart: Path) -> str:
    output = _run([str(dart), "--version"])
    match = _DART_VERSION_RE.search(output)
    if match is None:
        raise RuntimeError(f"cannot parse Dart SDK version from: {output.strip()}")
    return match.group("version")


def _absolute_root_uri(package_config: Path, root_uri: str) -> str:
    resolved = urljoin(package_config.resolve().as_uri(), root_uri)
    parsed = urlparse(resolved)
    if parsed.scheme != "file":
        raise ValueError(f"compiler oracle requires a local package root: {root_uri}")
    return Path(unquote(parsed.path)).resolve().as_uri()


def _external_package_entry(package_config: Path, name: str) -> dict[str, object]:
    document = json.loads(package_config.read_text())
    packages = document.get("packages")
    if not isinstance(packages, list):
        raise ValueError("app package_config.json has no packages array")
    matches = [entry for entry in packages if isinstance(entry, dict) and entry.get("name") == name]
    if len(matches) != 1:
        raise ValueError(f"app package_config.json does not identify exactly one {name!r} package")
    source = matches[0]
    root_uri = source.get("rootUri")
    package_uri = source.get("packageUri", "lib/")
    if not isinstance(root_uri, str) or not isinstance(package_uri, str):
        raise ValueError(f"invalid {name!r} package_config entry")
    entry: dict[str, object] = {
        "name": name,
        "rootUri": _absolute_root_uri(package_config, root_uri) + "/",
        "packageUri": package_uri,
    }
    language_version = source.get("languageVersion")
    if isinstance(language_version, str):
        entry["languageVersion"] = language_version
    return entry


def _package_language_version(pubspec: Path) -> str:
    match = _SDK_CONSTRAINT_RE.search(pubspec.read_text())
    if match is None:
        raise ValueError(f"cannot derive Dart language version from {pubspec}")
    return f"{match.group('major')}.{match.group('minor')}"


def _extract_internal_packages(sdk_repo: Path, sdk_version: str, destination: Path) -> None:
    archive = sp.run(
        [
            "git",
            "-C",
            str(sdk_repo),
            "archive",
            "--format=tar",
            sdk_version,
            *[f"pkg/{name}" for name in _INTERNAL_PACKAGES],
        ],
        check=False,
        stdout=sp.PIPE,
        stderr=sp.PIPE,
    )
    if archive.returncode != 0:
        raise RuntimeError(
            f"cannot materialize Dart SDK tag {sdk_version}: "
            f"{archive.stderr.decode(errors='replace')}"
        )
    with tarfile.open(fileobj=io.BytesIO(archive.stdout), mode="r:") as tar:
        tar.extractall(destination, filter="data")


def _write_package_config(
    path: Path,
    extracted_sdk: Path,
    app_package_config: Path,
) -> None:
    packages: list[dict[str, object]] = []
    for name in _INTERNAL_PACKAGES:
        root = (extracted_sdk / "pkg" / name).resolve()
        if not (root / "pubspec.yaml").is_file():
            raise FileNotFoundError(f"Dart SDK package {name!r} was not extracted")
        packages.append(
            {
                "name": name,
                "rootUri": root.as_uri() + "/",
                "packageUri": "lib/",
                # Use the package's exact tag-era language floor. This bypasses
                # pub's unrelated SDK-package dependency constraints without
                # parsing newer SDK source in an artificially old language mode.
                "languageVersion": _package_language_version(root / "pubspec.yaml"),
            }
        )

    # vm/unboxing_info imports the SDK type-flow utilities, which use
    # package:collection. Reuse the exact collection package already resolved
    # for the target Flutter application instead of asking pub for a new graph.
    packages.append(_external_package_entry(app_package_config, "collection"))
    path.write_text(json.dumps({"configVersion": 2, "packages": packages}, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run DartPlant's vm.unboxing-info.metadata oracle with exact SDK sources"
    )
    parser.add_argument("--dart", type=Path, required=True)
    parser.add_argument("--sdk-repo", type=Path, required=True)
    parser.add_argument("--app-package-config", type=Path, required=True)
    parser.add_argument("--dill", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not args.dart.is_file() or not args.dill.is_file() or not args.app_package_config.is_file():
        raise FileNotFoundError("compiler oracle Dart/DILL/package_config input is missing")
    if not (args.sdk_repo / ".git").exists():
        raise FileNotFoundError(f"Dart SDK source checkout is not a git repository: {args.sdk_repo}")

    sdk_version = _dart_version(args.dart)
    dump_script = Path(__file__).with_name("dump_abi_oracle.dart").resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dartplant-sdk-oracle-") as temp:
        extracted = Path(temp) / "sdk"
        extracted.mkdir()
        _extract_internal_packages(args.sdk_repo.resolve(), sdk_version, extracted)
        package_config = Path(temp) / "package_config.json"
        _write_package_config(package_config, extracted, args.app_package_config.resolve())
        _run(
            [
                str(args.dart.resolve()),
                f"--packages={package_config}",
                str(dump_script),
                str(args.dill.resolve()),
                str(args.output.resolve()),
            ]
        )
    print(f"compiler ABI oracle: Dart {sdk_version} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
