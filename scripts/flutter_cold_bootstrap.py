from __future__ import annotations

import os
import re
import shutil
import subprocess as sp
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path

from util import ROOT_DIR, adb_cmd, find_arm64_device, run


FIXTURE_DIR = ROOT_DIR / "tests" / "flutter_fixture"
APK_PATH = FIXTURE_DIR / "build" / "app" / "outputs" / "flutter-apk" / "app-release.apk"
GENERATED_DIR = FIXTURE_DIR / ".dart_tool" / "dartplant" / "generated"
SIDECAR_HEADER = GENERATED_DIR / "ordinary_aot_sidecar.h"
ABI_ORACLE_JSON = GENERATED_DIR / "abi_oracle.json"
CLOSURE_SIDECAR_HEADER = GENERATED_DIR / "p6_forced_stack_closure_sidecar.h"
P6_SIDECARS = (
    ("verifiedAbiInt64", "DartPlantP6Int64", GENERATED_DIR / "p6_int64_sidecar.h"),
    (
        "verifiedAbiEntryStack",
        "DartPlantP6EntryStack",
        GENERATED_DIR / "p6_entry_stack_sidecar.h",
    ),
    (
        "verifiedAbiOddStack",
        "DartPlantP6OddStack",
        GENERATED_DIR / "p6_odd_stack_sidecar.h",
    ),
    (
        "verifiedAbiThrowingStack",
        "DartPlantP6ThrowingStack",
        GENERATED_DIR / "p6_throwing_stack_sidecar.h",
    ),
    (
        "verifiedAbiForcedStack",
        "DartPlantP6ForcedStack",
        GENERATED_DIR / "p6_forced_stack_sidecar.h",
    ),
    ("verifiedAbiPair", "DartPlantP6Pair", GENERATED_DIR / "p6_pair_sidecar.h"),
)
PACKAGE = "dev.dartplant.dartplant_fixture"
ACTIVITY = f"{PACKAGE}/.MainActivity"

_BOOTSTRAP_RE = re.compile(
    r"cold bootstrap status=(?P<status>-?\d+).*?"
    r"rounds=(?P<rounds>\d+).*?sampled=(?P<sampled>\d+).*?"
    r"captured=(?P<captured>\d+).*?validated=(?P<validated>\d+).*?"
    r"send_fail=(?P<send_fail>\d+).*?timeout=(?P<timeout>\d+).*?"
    r"dart_pc=(?P<dart_pc>\d+)"
)


@dataclass(frozen=True)
class ColdStartResult:
    round_index: int
    rounds: int
    sampled: int
    captured: int
    validated: int
    dart_pc: int


def _capture(cmd: list[str], *, timeout: float | None = None) -> str:
    result = sp.run(
        cmd,
        check=False,
        stdout=sp.PIPE,
        stderr=sp.STDOUT,
        text=True,
        timeout=timeout,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed with exit code {result.returncode}: {cmd}\n{result.stdout}"
        )
    return result.stdout


def _resolve_flutter(flutter: str | None) -> str:
    candidate = flutter or os.getenv("FLUTTER_BIN") or shutil.which("flutter")
    if not candidate:
        raise RuntimeError("Flutter executable not found; pass --flutter or set FLUTTER_BIN")
    return str(Path(candidate).expanduser())


def _build_fixture(flutter: str, *, dobby_root: Path | None = None) -> None:
    build_env = os.environ.copy()
    resolved_dobby_root = (
        dobby_root.expanduser().resolve()
        if dobby_root is not None
        else (ROOT_DIR / "third_party" / "dobby").resolve()
    )
    if not resolved_dobby_root.joinpath("CMakeLists.txt").is_file():
        raise FileNotFoundError(f"Dobby source tree not found: {resolved_dobby_root}")
    # Always pass the root so Gradle/CMake cannot accidentally reuse a cached
    # external backend from a previous compatibility run.
    build_env["DARTPLANT_DOBBY_ROOT"] = str(resolved_dobby_root)
    run(
        [sys.executable, str(ROOT_DIR / "scripts" / "main.py"), "build", "host"],
        cwd=ROOT_DIR,
        env=build_env,
    )
    aot_analyzer = ROOT_DIR / "build" / "host" / "dartplant_aot_abi_analyzer_cli"
    if not aot_analyzer.is_file():
        raise FileNotFoundError(f"DartPlant ARM64 structural analyzer not found: {aot_analyzer}")
    GENERATED_DIR.mkdir(parents=True, exist_ok=True)
    SIDECAR_HEADER.write_text(
        "// Generated placeholder; replaced after the first AOT build.\n"
        "#pragma once\n"
        "#define DARTPLANT_ORDINARY_AOT_SIDECAR_AVAILABLE 0\n"
    )
    for _, _, header in P6_SIDECARS:
        header.write_text(
            "// Generated placeholder; replaced after the first AOT build.\n"
            "#pragma once\n"
        )
    CLOSURE_SIDECAR_HEADER.write_text(
        "// Generated placeholder; replaced after the first AOT build.\n"
        "#pragma once\n"
    )
    run([flutter, "pub", "get"], cwd=FIXTURE_DIR, env=build_env)
    build_command = [
        flutter,
        "build",
        "apk",
        "--release",
        "--target-platform",
        "android-arm64",
    ]
    run(build_command, cwd=FIXTURE_DIR, env=build_env)
    if not APK_PATH.is_file():
        raise FileNotFoundError(f"Flutter release APK was not produced: {APK_PATH}")

    dill_candidates = sorted(
        (FIXTURE_DIR / ".dart_tool" / "flutter_build").glob("*/app.dill"),
        key=lambda path: path.stat().st_mtime_ns,
        reverse=True,
    )
    if not dill_candidates:
        raise FileNotFoundError("Flutter release build did not leave an app.dill for the oracle")
    dill = GENERATED_DIR / "oracle_app.dill"
    shutil.copy2(dill_candidates[0], dill)
    libapp = GENERATED_DIR / "libapp.so"
    with zipfile.ZipFile(APK_PATH) as archive:
        libapp.write_bytes(archive.read("lib/arm64-v8a/libapp.so"))

    flutter_root = Path(flutter).resolve().parent.parent
    gen_snapshot = (
        flutter_root
        / "bin"
        / "cache"
        / "artifacts"
        / "engine"
        / "android-arm64-release"
        / "linux-x64"
        / "gen_snapshot"
    )
    if not gen_snapshot.is_file():
        raise FileNotFoundError(f"Flutter ARM64 gen_snapshot not found: {gen_snapshot}")
    dart = flutter_root / "bin" / "cache" / "dart-sdk" / "bin" / "dart"
    if not dart.is_file():
        raise FileNotFoundError(f"Flutter Dart executable not found: {dart}")
    sdk_repo = ROOT_DIR.parent / "sdk"
    if not (sdk_repo / ".git").exists():
        raise FileNotFoundError(
            "compiler ABI oracle requires the Dart SDK source checkout at "
            f"{sdk_repo}"
        )
    run(
        [
            sys.executable,
            str(ROOT_DIR / "tools" / "compiler-oracle" / "run_abi_oracle.py"),
            "--dart",
            str(dart),
            "--sdk-repo",
            str(sdk_repo),
            "--app-package-config",
            str(FIXTURE_DIR / ".dart_tool" / "package_config.json"),
            "--dill",
            str(dill),
            "--output",
            str(ABI_ORACLE_JSON),
        ],
        cwd=ROOT_DIR,
        env=build_env,
    )
    for function_name, symbol_prefix, output_header in P6_SIDECARS:
        run(
            [
                sys.executable,
                str(ROOT_DIR / "tools" / "compiler-oracle" / "build_snapshot_sidecar.py"),
                "--gen-snapshot",
                str(gen_snapshot),
                "--dill",
                str(dill),
                "--libapp",
                str(libapp),
                "--library-uri",
                "package:dartplant_fixture/main.dart",
                "--class-name",
                "Global",
                "--function-name",
                function_name,
                "--abi-oracle-json",
                str(ABI_ORACLE_JSON),
                "--aot-analyzer",
                str(aot_analyzer),
                "--symbol-prefix",
                symbol_prefix,
                "--output-header",
                str(output_header),
            ],
            cwd=ROOT_DIR,
            env=os.environ.copy(),
        )
    run(
        [
            sys.executable,
            str(ROOT_DIR / "tools" / "compiler-oracle" / "build_snapshot_sidecar.py"),
            "--gen-snapshot",
            str(gen_snapshot),
            "--dill",
            str(dill),
            "--libapp",
            str(libapp),
            "--library-uri",
            "package:dartplant_fixture/main.dart",
            "--class-name",
            "Global",
            "--function-name",
            "verifiedAbiForcedStack",
            "--artifact-function-name",
            "[tear-off] verifiedAbiForcedStack",
            "--compiler-function-kind",
            "ImplicitClosureFunction",
            "--abi-oracle-json",
            str(ABI_ORACLE_JSON),
            "--aot-analyzer",
            str(aot_analyzer),
            "--symbol-prefix",
            "DartPlantP6ForcedStackClosure",
            "--output-header",
            str(CLOSURE_SIDECAR_HEADER),
        ],
        cwd=ROOT_DIR,
        env=os.environ.copy(),
    )
    run(
        [
            sys.executable,
            str(ROOT_DIR / "tools" / "compiler-oracle" / "build_snapshot_sidecar.py"),
            "--gen-snapshot",
            str(gen_snapshot),
            "--dill",
            str(dill),
            "--libapp",
            str(libapp),
            "--library-uri",
            "package:dartplant_fixture/main.dart",
            "--class-name",
            "Global",
            "--function-name",
            "verifiedAbiDouble",
            "--abi-oracle-json",
            str(ABI_ORACLE_JSON),
            "--aot-analyzer",
            str(aot_analyzer),
            "--output-header",
            str(SIDECAR_HEADER),
        ],
        cwd=ROOT_DIR,
        env=os.environ.copy(),
    )

    # The generated header changes only the native fixture bridge. Dart source
    # and app.dill are unchanged, so deterministic libapp.so remains the exact
    # artifact to which the sidecar above was bound.
    run(build_command, cwd=FIXTURE_DIR, env=os.environ.copy())
    with zipfile.ZipFile(APK_PATH) as archive:
        rebuilt_libapp = archive.read("lib/arm64-v8a/libapp.so")
    if rebuilt_libapp != libapp.read_bytes():
        raise RuntimeError(
            "second-stage native fixture rebuild changed libapp.so; generated sidecar is stale"
        )


def _assert_no_packaged_runtime_metadata() -> None:
    with zipfile.ZipFile(APK_PATH) as archive:
        entries = archive.namelist()
    forbidden = [
        entry
        for entry in entries
        if "dartplant" in entry.lower() and "metadata" in entry.lower()
    ]
    if forbidden:
        raise RuntimeError(
            "fixture unexpectedly packages a raw DartPlant metadata asset: " + ", ".join(forbidden)
        )


def _read_pid(serial: str) -> str:
    return _capture(adb_cmd(["shell", "pidof", PACKAGE], device=serial)).strip()


def _signal_is_caught(serial: str, pid: str, signal_number: int) -> bool:
    status = _capture(adb_cmd(["shell", "cat", f"/proc/{pid}/status"], device=serial))
    for line in status.splitlines():
        if not line.startswith("SigCgt:"):
            continue
        mask = int(line.split()[1], 16)
        return bool(mask & (1 << (signal_number - 1)))
    raise RuntimeError("SigCgt was not present in /proc/<pid>/status")


def _wait_for_logs(serial: str, pid: str, timeout_seconds: float) -> str:
    deadline = time.monotonic() + timeout_seconds
    latest = ""
    while time.monotonic() < deadline:
        latest = _capture(
            adb_cmd(["logcat", f"--pid={pid}", "-d", "-v", "brief"], device=serial)
        )
        if (
            "cold bootstrap status=" in latest
            and "DartPlant initialize status:" in latest
            and "DartPlant local gate real-Dart warmup:" in latest
            and "DartPlant FunctionType semantic probe:" in latest
            and "DartPlant FunctionType named semantic probe:" in latest
            and "DartPlant bool semantic probe:" in latest
            and "DartPlant live VM startup probe:" in latest
            and "DartPlant closure receiver probe:" in latest
            and "DartPlant P6 ABI corpus:" in latest
            and "DartPlant ordinary AOT typed probe:" in latest
            and "DartPlant late shared typed fail-close:" in latest
        ):
            return latest
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for cold-bootstrap logs for pid {pid}\n{latest}")


def _validate_round(serial: str, round_index: int, timeout_seconds: float) -> ColdStartResult:
    run(adb_cmd(["logcat", "-c"], device=serial))
    run(adb_cmd(["shell", "am", "force-stop", PACKAGE], device=serial))
    run(adb_cmd(["shell", "am", "start", "-W", "-n", ACTIVITY], device=serial))

    pid = _read_pid(serial)
    if not pid:
        raise RuntimeError(f"cold start {round_index}: fixture process is not running")
    logs = _wait_for_logs(serial, pid, timeout_seconds)

    bootstrap_match = _BOOTSTRAP_RE.search(logs)
    if bootstrap_match is None:
        raise RuntimeError(f"cold start {round_index}: bootstrap diagnostics missing\n{logs}")
    values = {name: int(value) for name, value in bootstrap_match.groupdict().items()}
    if values["status"] != 0:
        raise RuntimeError(f"cold start {round_index}: bootstrap status={values['status']}\n{logs}")
    if values["validated"] < 1:
        raise RuntimeError(f"cold start {round_index}: no VM candidate validated\n{logs}")
    if values["send_fail"] != 0 or values["timeout"] != 0:
        raise RuntimeError(
            f"cold start {round_index}: sampler send_fail={values['send_fail']} "
            f"timeout={values['timeout']}\n{logs}"
        )
    if "live_index_ready=1" not in logs:
        raise RuntimeError(f"cold start {round_index}: automatic live Function index was not ready\n{logs}")
    if "DartPlant initialize status: 0" not in logs:
        raise RuntimeError(f"cold start {round_index}: runtime init failed\n{logs}")
    if "DartPlant local gate real-Dart warmup: 1 value=115" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: local-gated real Dart entry/JumpToFrame warmup failed\n{logs}"
        )
    if "DartPlant simple facade install: 0" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: simple facade current-thread bootstrap failed\n{logs}"
        )
    if "DartPlant simple facade typed hook: 1 values=27.625/29.25 stages=1/1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: simple typed facade lifecycle failed\n{logs}"
        )
    if "entry-family instrumentedAdd mask=0xf" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: four-kind Dart Code entry family was not proven\n{logs}"
        )
    if (
        "artifact closure hook status=0 source_offline=1 kind=2 default_only=1 receiver_x0=1"
        not in logs
    ):
        raise RuntimeError(
            f"cold start {round_index}: artifact closure hook installation failed\n{logs}"
        )
    if (
        "artifact closure receiver probe enter=1 failures=0 source_offline=1 receiver_x0=1 "
        "active=1 passed=1"
        not in logs
    ):
        raise RuntimeError(
            f"cold start {round_index}: native closure receiver contract failed\n{logs}"
        )
    if "DartPlant closure receiver probe: 1 value=78 native=1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: Dart implicit closure invocation failed\n{logs}"
        )
    if "simple facade typed install ready=1 status=0" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: simple facade lazy bootstrap/artifact hook failed\n{logs}"
        )
    if "simple facade typed stage1 enter=1 leave=1 observer=1 failures=0" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: first logical HookHandle removal failed\n{logs}"
        )
    if "simple facade typed stage2 enter=2 leave=2 observer=1 failures=0" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: final logical HookHandle removal failed\n{logs}"
        )
    if "P6 ABI install ready=1 status=0" not in logs:
        raise RuntimeError(f"cold start {round_index}: P6 artifact hook install failed\n{logs}")
    if (
        "P6 ABI probe int64=1 entry_stack=1 odd_stack=1 throw=1 forced_stack=1 pair=1 failures=0 "
        "cleanup=1 shutdown=1 passed=1"
        not in logs
    ):
        raise RuntimeError(f"cold start {round_index}: P6 native ABI probe failed\n{logs}")
    if "DartPlant P6 throw path: 2 normal=108.0/125.0" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: Dart exception unwind/catch probe failed\n{logs}"
        )
    if (
        "DartPlant P6 ABI corpus: 1 install=0 probe=1 "
        "int64=1000000000000000007/3000000010000000113 "
        "stack=108.0/1146.0 odd=91.0/217.0 forced=34/65 pair=21,22/32,31"
        not in logs
    ):
        raise RuntimeError(f"cold start {round_index}: P6 Dart result corpus failed\n{logs}")
    if (
        "exception bridge lifetime probe enter=1 leave=0 unhook=1 idle=1 inactive=1 "
        "failures=0 shutdown=1 passed=1"
        not in logs
    ):
        raise RuntimeError(
            f"cold start {round_index}: exception bridge self-unhook lifetime failed\n{logs}"
        )
    if "DartPlant exception bridge lifetime: 1 install=0 catch=2 probe=1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: Dart exception bridge lifetime result failed\n{logs}"
        )
    if "DartPlant advanced ordinary hook enable: 0" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: advanced ordinary hook handoff failed\n{logs}"
        )
    if "DartPlant live VM startup probe: 115" not in logs:
        raise RuntimeError(f"cold start {round_index}: hook probe did not return 115\n{logs}")
    if "DartPlant null semantic probe: 1 values=null/null" not in logs:
        raise RuntimeError(f"cold start {round_index}: null semantic probe failed\n{logs}")
    if "DartPlant FunctionType semantic probe: 1" not in logs:
        raise RuntimeError(f"cold start {round_index}: FunctionType semantic probe failed\n{logs}")
    if "DartPlant FunctionType named semantic probe: 1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: generic/named FunctionType semantic probe failed\n{logs}"
        )
    if "DartPlant bool semantic probe: 1 values=false/true" not in logs:
        raise RuntimeError(f"cold start {round_index}: bool semantic probe failed\n{logs}")
    if "DartPlant ordinary AOT discovery: 1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: ordinary AOT Function discovery failed\n{logs}"
        )
    required_ordinary_markers = (
        "evidence_status=0",
        "abi_info_status=0",
        "abi_state=2",
        "verified_layout=1",
        "hook_status=0",
        "observer_hook_status=0",
        "source_offline=1",
    )
    if any(marker not in logs for marker in required_ordinary_markers):
        raise RuntimeError(
            f"cold start {round_index}: ordinary AOT compiler ABI binding failed\n{logs}"
        )
    if "source_offline=1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: ordinary AOT lookup did not use the artifact index\n{logs}"
        )
    if "DartPlant ordinary AOT typed probe: 1 values=16.125/6.25" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: ordinary AOT typed callback probe failed\n{logs}"
        )
    if (
        "verified ordinary AOT double probe enter=1 leave=1 observer=1 failures=0 passed=1"
        not in logs
    ):
        raise RuntimeError(
            f"cold start {round_index}: logical HookHandle subscription sharing failed\n{logs}"
        )
    if "DartPlant late shared transition: 1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: late shared entry-target transition failed\n{logs}"
        )
    if "DartPlant late shared typed fail-close: 1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: late shared callback fail-close failed\n{logs}"
        )
    if "runtime live-vm lookup addInt ok" not in logs or "model_ok=1" not in logs:
        raise RuntimeError(f"cold start {round_index}: live model regression failed\n{logs}")
    required_shared_markers = (
        "explicit_opt_in=1",
        "ambiguous_identity=1",
        "second_listener_identity=1",
        "aliases=2",
        "known_aliases=2",
    )
    if any(marker not in logs for marker in required_shared_markers):
        raise RuntimeError(
            f"cold start {round_index}: shared-code callback semantics failed\n{logs}"
        )
    if "Fatal signal" in logs or "FATAL EXCEPTION" in logs:
        raise RuntimeError(f"cold start {round_index}: process reported a fatal failure\n{logs}")

    # The bootstrap may temporarily borrow SIGWINCH (28) or SIGURG (23), but
    # neither signal is allowed to remain caught after discovery completes.
    if _signal_is_caught(serial, pid, 28) or _signal_is_caught(serial, pid, 23):
        raise RuntimeError(f"cold start {round_index}: sampling signal handler was not restored")

    return ColdStartResult(
        round_index=round_index,
        rounds=values["rounds"],
        sampled=values["sampled"],
        captured=values["captured"],
        validated=values["validated"],
        dart_pc=values["dart_pc"],
    )


def run_flutter_cold_bootstrap_test(
    *,
    device: str | None,
    flutter: str | None,
    rounds: int,
    timeout_seconds: float,
    build: bool,
    dobby_root: Path | None = None,
) -> None:
    if rounds <= 0:
        raise ValueError("rounds must be greater than zero")
    if timeout_seconds <= 0:
        raise ValueError("timeout must be greater than zero")

    flutter_bin = _resolve_flutter(flutter) if build else ""
    if build:
        _build_fixture(flutter_bin, dobby_root=dobby_root)
    if not APK_PATH.is_file():
        raise FileNotFoundError(f"missing Flutter fixture APK: {APK_PATH}")
    _assert_no_packaged_runtime_metadata()

    serial = find_arm64_device(device)
    run(adb_cmd(["install", "-r", str(APK_PATH)], device=serial))

    results = [
        _validate_round(serial, index, timeout_seconds) for index in range(1, rounds + 1)
    ]
    sampled = [result.sampled for result in results]
    no_dart_pc = sum(result.dart_pc == 0 for result in results)
    print(
        "hybrid live/artifact cold bootstrap: "
        f"{len(results)}/{rounds} passed; "
        f"sampled min={min(sampled)} max={max(sampled)}; "
        f"validated={sum(result.validated for result in results)}; "
        f"dart_pc_zero_rounds={no_dart_pc}"
    )
