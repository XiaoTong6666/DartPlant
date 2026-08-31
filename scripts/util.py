from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess as sp
import re

from dataclasses import dataclass
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parent.parent
CONFIG_PATH = ROOT_DIR / "project-config.json"
BUILD_ROOT = ROOT_DIR / "build"

BUILD_TYPE_TO_CMAKE = {
    "debug": "Debug",
    "release": "RelWithDebInfo",
}


@dataclass(frozen=True)
class ProjectConfig:
    project_name: str
    platform: str
    ndk_version: str
    abi: str
    device_directory: str


@dataclass(frozen=True)
class BuildContext:
    config: ProjectConfig
    android_home: Path
    ndk_home: Path
    build_type: str
    cmake_build_type: str

    @property
    def host_build_dir(self) -> Path:
        return BUILD_ROOT / "host"

    @property
    def android_build_dir(self) -> Path:
        return BUILD_ROOT / "android-arm64"

def run(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    ignore_error: bool = False,
) -> None:
    print("+", " ".join(shlex.quote(part) for part in cmd))
    result = sp.run(cmd, cwd=cwd, env=env, check=False)
    if not ignore_error and result.returncode != 0:
        raise RuntimeError(
            f"command failed with exit code {result.returncode}: {cmd}"
        )


def capture(cmd: list[str], *, cwd: Path | None = None) -> str:
    result = sp.run(
        cmd,
        cwd=cwd,
        check=True,
        stdout=sp.PIPE,
        stderr=sp.DEVNULL,
        text=True,
    )
    return result.stdout.strip()


def capture_or_default(
    cmd: list[str], default: str, *, cwd: Path | None = None
) -> str:
    try:
        return capture(cmd, cwd=cwd) or default
    except Exception:
        return default


def load_config() -> ProjectConfig:
    with CONFIG_PATH.open("r", encoding="utf-8") as fp:
        data = json.load(fp)
    return ProjectConfig(
        project_name=data["projectName"],
        platform=data["platform"],
        ndk_version=data["ndkVer"],
        abi=data["abi"],
        device_directory=data["deviceDirectory"],
    )


def resolve_android_home() -> Path:
    value = os.getenv("ANDROID_HOME") or os.getenv("ANDROID_SDK_ROOT")
    if value:
        return Path(value).expanduser().resolve()
    for candidate in (Path.home() / "Android" / "Sdk", Path.home() / "Library" / "Android" / "sdk"):
        if candidate.is_dir():
            return candidate.resolve()
    raise ValueError("ANDROID_HOME or ANDROID_SDK_ROOT is required")


def create_context(build_type: str, ndk_override: str | None = None) -> BuildContext:
    if build_type not in BUILD_TYPE_TO_CMAKE:
        raise ValueError(f"unsupported build type: {build_type}")
    config = load_config()
    android_home = resolve_android_home()
    ndk_home = android_home / "ndk" / (ndk_override or config.ndk_version)
    if not ndk_home.is_dir():
        raise ValueError(f"Android NDK not found: {ndk_home}")
    return BuildContext(
        config=config,
        android_home=android_home,
        ndk_home=ndk_home,
        build_type=build_type,
        cmake_build_type=BUILD_TYPE_TO_CMAKE[build_type],
    )


def cmake_build(build_dir: Path) -> None:
    run(
        ["cmake", "--build", str(build_dir), "--", f"-j{os.cpu_count() or 4}"],
        cwd=ROOT_DIR,
    )


def build_host(ctx: BuildContext, *, force: bool = False) -> None:
    if force:
        shutil.rmtree(ctx.host_build_dir, ignore_errors=True)
    run(
        [
            "cmake",
            "-S",
            str(ROOT_DIR),
            "-B",
            str(ctx.host_build_dir),
            "-G",
            "Ninja",
            f"-DCMAKE_BUILD_TYPE={ctx.cmake_build_type}",
            "-DDARTPLANT_BUILD_TESTS=ON",
            "-DDARTPLANT_BUILD_ANDROID_TESTS=OFF",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ],
        cwd=ROOT_DIR,
    )
    cmake_build(ctx.host_build_dir)


def _android_build_dir_for_dobby(ctx: BuildContext, dobby_root: Path) -> Path:
    default_root = (ROOT_DIR / "third_party" / "dobby").resolve()
    if dobby_root.resolve() == default_root:
        return ctx.android_build_dir
    owner = dobby_root.resolve().parent.parent.name.lower() or "external"
    label = re.sub(r"[^a-z0-9]+", "-", owner).strip("-") or "external"
    return BUILD_ROOT / f"android-arm64-{label}"


def build_android(
    ctx: BuildContext, *, force: bool = False, dobby_root_override: Path | None = None
) -> Path:
    if ctx.config.abi != "arm64-v8a":
        raise ValueError("DartPlant currently supports arm64-v8a only")
    dobby_root = (
        dobby_root_override.expanduser().resolve()
        if dobby_root_override is not None
        else (ROOT_DIR / "third_party" / "dobby").resolve()
    )
    build_dir = _android_build_dir_for_dobby(ctx, dobby_root)
    if not dobby_root.joinpath("CMakeLists.txt").is_file():
        raise FileNotFoundError(
            "Dobby submodule is missing; run git submodule update --init --recursive"
        )
    if force:
        shutil.rmtree(build_dir, ignore_errors=True)
    run(
        [
            "cmake",
            "-S",
            str(ROOT_DIR),
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={ctx.ndk_home / 'build/cmake/android.toolchain.cmake'}",
            f"-DANDROID_ABI={ctx.config.abi}",
            f"-DANDROID_PLATFORM={ctx.config.platform}",
            f"-DCMAKE_BUILD_TYPE={ctx.cmake_build_type}",
            "-DDARTPLANT_BUILD_TESTS=OFF",
            "-DDARTPLANT_BUILD_ANDROID_TESTS=ON",
            "-DDARTPLANT_BUILD_ANDROID_MODULE=OFF",
            f"-DDARTPLANT_DOBBY_ROOT={dobby_root}",
            "-DANDROID_STL=c++_static",
            "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ],
        cwd=ROOT_DIR,
    )
    cmake_build(build_dir)
    return build_dir

def build_all(ctx: BuildContext, *, force: bool = False) -> None:
    build_host(ctx, force=force)
    build_android(ctx, force=force)


def test_host(ctx: BuildContext, *, force: bool = False) -> None:
    build_host(ctx, force=force)
    run(["python3", str(ROOT_DIR / "tests" / "public_c_header_test.py")], cwd=ROOT_DIR)
    run(["python3", str(ROOT_DIR / "tests" / "loader_compat_test.py")], cwd=ROOT_DIR)
    run(["python3", str(ROOT_DIR / "tests" / "vm_profiles_generator_test.py")], cwd=ROOT_DIR)
    run(["python3", str(ROOT_DIR / "scripts" / "generate_vm_profiles.py"), "--check"], cwd=ROOT_DIR)
    run(["dart", "pub", "get"], cwd=ROOT_DIR / "tests" / "dart")
    run(["dart", "test"], cwd=ROOT_DIR / "tests" / "dart")
    run(
        ["python3", str(ROOT_DIR / "tools" / "compiler-oracle" / "test_build_snapshot_sidecar.py")],
        cwd=ROOT_DIR,
    )
    run(["python3", str(ROOT_DIR / "tests" / "cmake_parent_smoke.py")], cwd=ROOT_DIR)
    run(
        [
            "cargo",
            "test",
            "--manifest-path",
            str(ROOT_DIR / "tools" / "dartplant-indexer" / "Cargo.toml"),
        ],
        cwd=ROOT_DIR,
        env={
            **os.environ,
            "CC": "gcc",
            "CXX": "g++",
            "CARGO_TARGET_DIR": str(BUILD_ROOT / "tools"),
        },
    )
    run(
        ["ctest", "--test-dir", str(ctx.host_build_dir), "--output-on-failure"],
        cwd=ROOT_DIR,
    )


def adb_cmd(args: list[str], *, device: str | None = None) -> list[str]:
    command = ["adb"]
    if device:
        command += ["-s", device]
    return command + args


def find_arm64_device(device: str | None = None) -> str:
    if device:
        abi = capture(adb_cmd(["shell", "getprop", "ro.product.cpu.abi"], device=device))
        if abi != "arm64-v8a":
            raise ValueError(f"device {device} is not arm64-v8a: {abi}")
        return device
    output = capture(["adb", "devices"])
    for line in output.splitlines()[1:]:
        fields = line.split()
        if len(fields) != 2 or fields[1] != "device":
            continue
        serial = fields[0]
        abi = capture(adb_cmd(["shell", "getprop", "ro.product.cpu.abi"], device=serial))
        if abi == "arm64-v8a":
            return serial
    raise RuntimeError("no attached arm64-v8a device found")


def test_device(
    ctx: BuildContext,
    *,
    device: str | None = None,
    force: bool = False,
    dobby_root: Path | None = None,
) -> None:
    build_dir = build_android(ctx, force=force, dobby_root_override=dobby_root)
    serial = find_arm64_device(device)
    remote = ctx.config.device_directory
    artifacts = [
        build_dir / "dartplant_device_tests",
        build_dir / "libdartplant_device_fixture.so",
    ]
    # Some external Dobby trees (notably current Vector) default to a shared
    # backend. Push it alongside the test binary when present; Irena/default
    # builds are static and need no extra runtime artifact.
    external_dobby = build_dir / "third_party" / "dobby" / "libdobby.so"
    if external_dobby.is_file():
        artifacts.append(external_dobby)
    for artifact in artifacts:
        if not artifact.is_file():
            raise FileNotFoundError(f"missing Android artifact: {artifact}")
    run(adb_cmd(["shell", "rm", "-rf", remote], device=serial))
    run(adb_cmd(["shell", "mkdir", "-p", remote], device=serial))
    run(adb_cmd(["push", *[str(path) for path in artifacts], f"{remote}/"], device=serial))
    run(adb_cmd(["shell", "chmod", "0755", f"{remote}/dartplant_device_tests"], device=serial))
    run(
        adb_cmd(
            [
                "shell",
                f"cd {shlex.quote(remote)} && "
                f"LD_LIBRARY_PATH={shlex.quote(remote)} ./dartplant_device_tests",
            ],
            device=serial,
        )
    )


def clean_outputs() -> None:
    shutil.rmtree(BUILD_ROOT, ignore_errors=True)


def doctor_snapshot(ndk_override: str | None = None) -> dict[str, str]:
    ctx = create_context("debug", ndk_override=ndk_override)
    return {
        "root": str(ROOT_DIR),
        "android_home": str(ctx.android_home),
        "ndk_home": str(ctx.ndk_home),
        "cmake": shutil.which("cmake") or "missing",
        "ninja": shutil.which("ninja") or "missing",
        "adb": shutil.which("adb") or "missing",
        "abi": ctx.config.abi,
        "dobby_root": str(ROOT_DIR / "third_party" / "dobby"),
    }
