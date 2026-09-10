from __future__ import annotations

import json
import os
import re
import shutil
import subprocess as sp
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path

from ci.capability_registry import mask as capability_mask
from ci.capability_registry import required_event_capabilities
from util import ROOT_DIR, adb_cmd, find_arm64_device, run


FIXTURE_DIR = ROOT_DIR / "tests" / "flutter_fixture"
GENERATED_DIR = FIXTURE_DIR / ".dart_tool" / "dartplant" / "generated"
SIDECAR_HEADER = GENERATED_DIR / "ordinary_aot_sidecar.h"
ABI_ORACLE_JSON = GENERATED_DIR / "abi_oracle.json"
CLOSURE_SIDECAR_HEADER = GENERATED_DIR / "p6_forced_stack_closure_sidecar.h"
TYPE_ARGUMENTS_CLOSURE_SIDECAR_HEADER = GENERATED_DIR / "type_arguments_closure_sidecar.h"
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


@dataclass(frozen=True)
class FlutterToolchain:
    flutter_version: str
    dart_version: str


def _normalize_flutter_mode(mode: str) -> str:
    if mode not in {"release", "profile"}:
        raise ValueError(f"unsupported Flutter AOT mode: {mode}")
    return mode


def flutter_fixture_apk_path(mode: str) -> Path:
    mode = _normalize_flutter_mode(mode)
    return (
        FIXTURE_DIR
        / "build"
        / "app"
        / "outputs"
        / "flutter-apk"
        / f"app-{mode}.apk"
    )


def flutter_gen_snapshot_path(flutter: str, mode: str) -> Path:
    mode = _normalize_flutter_mode(mode)
    flutter_root = Path(flutter).resolve().parent.parent
    return (
        flutter_root
        / "bin"
        / "cache"
        / "artifacts"
        / "engine"
        / f"android-arm64-{mode}"
        / "linux-x64"
        / "gen_snapshot"
    )


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


def _detect_flutter_toolchain(flutter: str) -> FlutterToolchain:
    try:
        output = _capture([flutter, "--version", "--machine"])
        json_start = output.find("{")
        if json_start < 0:
            raise json.JSONDecodeError("missing JSON object", output, 0)
        machine, _ = json.JSONDecoder().raw_decode(output[json_start:])
    except (json.JSONDecodeError, TypeError) as error:
        raise RuntimeError("Flutter --version --machine did not return valid JSON") from error
    flutter_version = str(machine.get("frameworkVersion", "")).strip()
    dart_text = str(machine.get("dartSdkVersion", "")).strip()
    dart_match = re.search(r"\d+\.\d+\.\d+", dart_text)
    if not flutter_version or dart_match is None:
        raise RuntimeError(
            "Flutter toolchain version metadata is incomplete: "
            f"frameworkVersion={flutter_version!r} dartSdkVersion={dart_text!r}"
        )
    return FlutterToolchain(
        flutter_version=flutter_version,
        dart_version=dart_match.group(0),
    )


def _build_fixture(
    flutter: str, *, dobby_root: Path | None = None, build_mode: str = "release"
) -> FlutterToolchain:
    build_mode = _normalize_flutter_mode(build_mode)
    toolchain = _detect_flutter_toolchain(flutter)
    build_env = os.environ.copy()
    # The fixture is deliberately cleaned and rebuilt with several Flutter
    # toolchains in one checkout. Gradle's file-system watcher can retain
    # stale snapshots across those destructive clean/rebuild boundaries and
    # has produced disappearing R8/mergeJavaResource incremental inputs on the
    # development host. Disable VFS watching and the persistent Gradle daemon
    # for this deterministic compiler-oracle build; neither setting changes
    # Dart AOT or APK contents.
    gradle_opts = build_env.get("GRADLE_OPTS", "").strip()
    deterministic_gradle_opts = (
        "-Dorg.gradle.vfs.watch=false -Dorg.gradle.daemon=false"
    )
    build_env["GRADLE_OPTS"] = " ".join(
        part for part in (gradle_opts, deterministic_gradle_opts) if part
    )
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

    # A release build may reuse .dart_tool/flutter_build entries produced by a
    # different Flutter/Dart toolchain. Picking the newest app.dill by mtime is
    # not sufficient in that case: an old Kernel binary can survive while the
    # APK itself is rebuilt, then fail the exact-version compiler oracle with a
    # Kernel format mismatch. Clean before recreating DartPlant's generated
    # sidecar placeholders so both the APK and oracle dill come from this exact
    # Flutter invocation.
    run([flutter, "clean"], cwd=FIXTURE_DIR, env=build_env)
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
    TYPE_ARGUMENTS_CLOSURE_SIDECAR_HEADER.write_text(
        "// Generated placeholder; replaced after the first AOT build.\n"
        "#pragma once\n"
    )
    run([flutter, "pub", "get"], cwd=FIXTURE_DIR, env=build_env)
    build_command = [
        flutter,
        "build",
        "apk",
        f"--{build_mode}",
        "--target-platform",
        "android-arm64",
        f"--dart-define=DARTPLANT_CI_FLUTTER_VERSION={toolchain.flutter_version}",
        f"--dart-define=DARTPLANT_CI_DART_VERSION={toolchain.dart_version}",
        "--dart-define=DARTPLANT_CI_TARGET_ABI=arm64-v8a",
    ]
    run(build_command, cwd=FIXTURE_DIR, env=build_env)
    apk_path = flutter_fixture_apk_path(build_mode)
    if not apk_path.is_file():
        raise FileNotFoundError(f"Flutter {build_mode} APK was not produced: {apk_path}")

    dill_candidates = sorted(
        (FIXTURE_DIR / ".dart_tool" / "flutter_build").glob("*/app.dill"),
        key=lambda path: path.stat().st_mtime_ns,
        reverse=True,
    )
    if not dill_candidates:
        raise FileNotFoundError(
            f"Flutter {build_mode} build did not leave an app.dill for the oracle"
        )
    dill = GENERATED_DIR / "oracle_app.dill"
    shutil.copy2(dill_candidates[0], dill)
    libapp = GENERATED_DIR / "libapp.so"
    with zipfile.ZipFile(apk_path) as archive:
        libapp.write_bytes(archive.read("lib/arm64-v8a/libapp.so"))

    flutter_root = Path(flutter).resolve().parent.parent
    gen_snapshot = flutter_gen_snapshot_path(flutter, build_mode)
    if not gen_snapshot.is_file():
        raise FileNotFoundError(
            f"Flutter ARM64 {build_mode} gen_snapshot not found: {gen_snapshot}"
        )
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
            "signatureProbe",
            "--artifact-function-name",
            "[tear-off] signatureProbe",
            "--compiler-function-kind",
            "ImplicitClosureFunction",
            "--abi-oracle-json",
            str(ABI_ORACLE_JSON),
            "--aot-analyzer",
            str(aot_analyzer),
            "--symbol-prefix",
            "DartPlantTypeArgumentsClosure",
            "--output-header",
            str(TYPE_ARGUMENTS_CLOSURE_SIDECAR_HEADER),
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

    # The generated headers change only the native fixture bridge. Gradle's
    # externalNativeBuild input snapshot does not reliably notice headers that
    # are created after the first configure/build, so a second Flutter build can
    # otherwise reuse the placeholder-built fixture_bridge object and omit the
    # static ArtifactBundle registrars entirely. Force only the native CMake
    # intermediates to be regenerated; Dart source/app.dill stay untouched and
    # deterministic libapp.so remains the exact artifact to which the sidecars
    # above were bound.
    shutil.rmtree(FIXTURE_DIR / "android" / "app" / ".cxx", ignore_errors=True)
    shutil.rmtree(FIXTURE_DIR / "build" / "app" / "intermediates" / "cxx", ignore_errors=True)
    run(build_command, cwd=FIXTURE_DIR, env=build_env)
    with zipfile.ZipFile(apk_path) as archive:
        rebuilt_libapp = archive.read("lib/arm64-v8a/libapp.so")
    if rebuilt_libapp != libapp.read_bytes():
        raise RuntimeError(
            "second-stage native fixture rebuild changed libapp.so; generated sidecar is stale"
        )
    return toolchain


def build_flutter_fixture(
    *, flutter: str | None, dobby_root: Path | None = None, build_mode: str = "release"
) -> FlutterToolchain:
    flutter_bin = _resolve_flutter(flutter)
    build_mode = _normalize_flutter_mode(build_mode)
    lock_path = FIXTURE_DIR / "pubspec.lock"
    lock_existed = lock_path.is_file()
    lock_contents = lock_path.read_bytes() if lock_existed else None
    try:
        return _build_fixture(flutter_bin, dobby_root=dobby_root, build_mode=build_mode)
    finally:
        # Compatibility builds intentionally resolve the dependency graph with
        # the active Flutter/Dart SDK. Do not leave those SDK-specific pins in
        # the source checkout after the fixture APK has been produced.
        if lock_existed and lock_contents is not None:
            lock_path.write_bytes(lock_contents)
        elif not lock_existed:
            lock_path.unlink(missing_ok=True)


def _assert_no_packaged_runtime_metadata(apk_path: Path) -> None:
    with zipfile.ZipFile(apk_path) as archive:
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
            and "DartPlant Flutter VM adapter profile gate:" in latest
            and "DartPlant local gate real-Dart warmup:" in latest
            and "DartPlant FunctionType semantic probe:" in latest
            and "DartPlant FunctionType named semantic probe:" in latest
            and "DartPlant bool semantic probe:" in latest
            and "DartPlant live VM startup probe:" in latest
            and "DartPlant closure receiver probe:" in latest
            # The app also runs a UI/bootstrap TypeArguments proof before the
            # adb-requested probe. Waiting for the generic prefix can therefore
            # return while the adb proof is still inside its GC pressure loop,
            # making the later source=adb assertion timing-dependent.
            and "DartPlant app TypeArguments proof: 1 source=adb native=1 require_relocation=1 result_ok=1"
            in latest
            and "DartPlant P6 ABI corpus:" in latest
            and "DartPlant ordinary AOT typed probe:" in latest
            and "DartPlant late shared typed fail-close:" in latest
            and 'DARTPLANT_CI {"event":"suite","state":"pass","test":"all"}' in latest
        ):
            return latest
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for cold-bootstrap logs for pid {pid}\n{latest}")


def _structured_events(logs: str, event_name: str) -> list[dict[str, object]]:
    prefix = "DARTPLANT_CI "
    events: list[dict[str, object]] = []
    for line in logs.splitlines():
        position = line.find(prefix)
        if position < 0:
            continue
        try:
            event = json.loads(line[position + len(prefix) :].strip())
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict) and event.get("event") == event_name:
            events.append(event)
    return events


def _required_ordinary_source_markers(build_mode: str) -> tuple[str, ...]:
    build_mode = _normalize_flutter_mode(build_mode)
    if build_mode == "profile":
        return (
            "source=live-vm+artifact-evidence",
            "source_live=1",
            "source_offline=0",
        )
    return (
        "source=artifact-index",
        "source_live=0",
        "source_offline=1",
    )


def _validate_round(
    serial: str, round_index: int, timeout_seconds: float, build_mode: str
) -> ColdStartResult:
    run(adb_cmd(["logcat", "-c"], device=serial))
    run(adb_cmd(["shell", "am", "force-stop", PACKAGE], device=serial))
    run(
        adb_cmd(
            [
                "shell",
                "am",
                "start",
                "-W",
                "-n",
                ACTIVITY,
                "--es",
                "dartplant_test",
                "all",
            ],
            device=serial,
        )
    )

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
    if "DartPlant Flutter VM adapter profile gate: 1 count=3" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: Flutter VM adapter profile gate failed\n{logs}"
        )
    dynamic_gate = re.search(
        r"DartPlant VM dynamic ABI gate: "
        r"capabilities=0x([0-9a-fA-F]+) required=0x([0-9a-fA-F]+) "
        r"verified=0x([0-9a-fA-F]+)/0x([0-9a-fA-F]+)/0x([0-9a-fA-F]+) "
        r"failed=0x([0-9a-fA-F]+)/0x([0-9a-fA-F]+) register_proven=(\d+) "
        r"artifact_revalidation=(-?\d+) artifact_generation=(\d+)/(\d+)/(\d+)",
        logs,
    )
    if dynamic_gate is None:
        raise RuntimeError(
            f"cold start {round_index}: dynamic VM ABI capability/lifecycle gate failed\n{logs}"
        )
    (
        capabilities_hex,
        required_hex,
        verified_before_hex,
        verified_invalidated_hex,
        verified_revalidated_hex,
        failed_before_hex,
        failed_revalidated_hex,
        register_proven,
        artifact_revalidation,
        generation_before,
        generation_invalidated,
        generation_revalidated,
    ) = dynamic_gate.groups()
    capabilities = int(capabilities_hex, 16)
    required = int(required_hex, 16)
    verified_before = int(verified_before_hex, 16)
    verified_invalidated = int(verified_invalidated_hex, 16)
    verified_revalidated = int(verified_revalidated_hex, 16)
    failed_before = int(failed_before_hex, 16)
    failed_revalidated = int(failed_revalidated_hex, 16)
    generation_before_value = int(generation_before)
    generation_invalidated_value = int(generation_invalidated)
    generation_revalidated_value = int(generation_revalidated)
    lifecycle_events = _structured_events(logs, "artifact_lifecycle")
    lifecycle_isolate_generations = [
        int(event.get("isolate_generation", 0))
        for event in lifecycle_events
        if int(event.get("generation_revalidated", -1)) == generation_revalidated_value
    ]
    current_isolate_generation = (
        lifecycle_isolate_generations[-1] if lifecycle_isolate_generations else 0
    )
    expected_capabilities = capability_mask(cold_required=True)
    # Adapter creation itself runs inside the Dart FFI/native transition, so
    # Generated->Native state is intentionally lazy-proven on the first real
    # generated callback. Create/revalidate therefore verify roots, safepoint
    # stubs and artifact identity, not the lazy transition state.
    expected_verified = capability_mask(verified_after_create=True)
    # Every private-ABI proof is bound to both artifact and isolate generation;
    # retirement clears the complete verified set before revalidation re-runs
    # eager core/object/stub proof.
    expected_invalidated = 0
    if not (
        required == expected_capabilities
        and (capabilities & required) == required
        and verified_before == expected_verified
        and verified_invalidated == expected_invalidated
        and verified_revalidated == expected_verified
        and failed_before == 0
        and failed_revalidated == 0
        and int(register_proven) == 0
        and int(artifact_revalidation) == 0
        and generation_before_value > 0
        and generation_invalidated_value == generation_before_value + 1
        and generation_revalidated_value == generation_invalidated_value
        and current_isolate_generation > 0
    ):
        raise RuntimeError(
            f"cold start {round_index}: dynamic VM ABI capability/lifecycle state mismatch\n{logs}"
        )
    if "result=pass stage=complete" not in logs or "dart_core=1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: source-candidate structural root proof missing\n{logs}"
        )
    capability_events = _structured_events(logs, "capability")
    for capability in required_event_capabilities():
        matching = [
            event
            for event in capability_events
            if event.get("capability") == capability.diagnostic_name
            and event.get("state") == "verified"
            and int(event.get("schema_version", 0)) >= 2
            and int(str(event.get("capability_bit", "0")), 0) == capability.bit
            and int(event.get("artifact_generation", -1)) == generation_revalidated_value
            and int(event.get("isolate_generation", -1)) == current_isolate_generation
        ]
        if not matching:
            raise RuntimeError(
                f"cold start {round_index}: {capability.diagnostic_name} lazy proof was not "
                f"verified for the current generation\n{logs}"
            )
    if any(
        event.get("state")
        in {"predicate_failed", "ambiguous", "generation_stale", "dependency_mismatch"}
        for event in capability_events
    ):
        raise RuntimeError(
            f"cold start {round_index}: a VM capability failed for the active incarnation\n{logs}"
        )
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
    if "DartPlant app launch probe: all" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: all-test launch extra was not delivered\n{logs}"
        )
    if "capture vector=" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: DartPlant did not log generated-state TypeArguments capture\n{logs}"
        )
    if "root_get element[0]" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: DartPlant did not read the TypeArguments element root\n{logs}"
        )
    if (
        "TypeArguments proof native result dart_api=1" not in logs
        or "parameter_relocated=1" not in logs
        or "passed=1" not in logs
    ):
        raise RuntimeError(
            f"cold start {round_index}: Tagged parameter was not proven across Dart API GC\n{logs}"
        )
    if "TypeArguments proof native summary enter=1 failures=0" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: native TypeArguments proof summary failed\n{logs}"
        )
    if (
        "DartPlant app TypeArguments proof: 1 source=adb native=1 require_relocation=1 result_ok=1"
        not in logs
    ):
        raise RuntimeError(
            f"cold start {round_index}: Dart-side TypeArguments proof result failed\n{logs}"
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
    if (
        'DARTPLANT_CI {"event":"scenario","name":"bool_semantics","state":"pass"'
        not in logs
        and "DartPlant bool semantic probe: 1 values=false/true" not in logs
    ):
        raise RuntimeError(f"cold start {round_index}: bool semantic probe failed\n{logs}")
    if "DartPlant ordinary AOT discovery: 1" not in logs:
        raise RuntimeError(
            f"cold start {round_index}: ordinary AOT Function discovery failed\n{logs}"
        )
    ordinary_lines = [
        line for line in logs.splitlines() if "DartPlant ordinary AOT discovery:" in line
    ]
    ordinary_line = ordinary_lines[-1] if ordinary_lines else ""
    required_ordinary_markers = (
        "DartPlant ordinary AOT discovery: 1",
        "evidence_status=0",
        "abi_info_status=0",
        "abi_state=2",
        "verified_layout=1",
        "hook_status=0",
        "observer_hook_status=0",
        *_required_ordinary_source_markers(build_mode),
    )
    if any(marker not in ordinary_line for marker in required_ordinary_markers):
        raise RuntimeError(
            f"cold start {round_index}: ordinary AOT compiler ABI binding failed\n{logs}"
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
    producer_lines = [
        line for line in logs.splitlines() if "producer code-identity policy verified mode=" in line
    ]
    probe_lines = [
        line for line in logs.splitlines() if "instrumentedAdd probe mode=" in line
    ]
    producer_line = producer_lines[-1] if producer_lines else ""
    probe_line = probe_lines[-1] if probe_lines else ""
    if "mode=no-dedup-unique" in producer_line:
        required_identity_markers = (
            "instrumented_aliases=1",
            "add_int_aliases=1",
        )
        required_probe_markers = (
            "mode=no-dedup-unique",
            "policy_ok=1",
            "ambiguous_identity=0",
            "second_listener_identity=0",
            "model_ok=1",
        )
    elif "mode=dedup-shared" in producer_line:
        required_identity_markers = (
            "instrumented_aliases=2",
            "add_int_aliases=2",
        )
        required_probe_markers = (
            "mode=dedup-shared",
            "policy_ok=1",
            "ambiguous_identity=1",
            "second_listener_identity=1",
            "model_ok=1",
        )
    else:
        raise RuntimeError(
            f"cold start {round_index}: producer code-identity mode was not proven\n{logs}"
        )
    if any(marker not in producer_line for marker in required_identity_markers) or any(
        marker not in probe_line for marker in required_probe_markers
    ):
        raise RuntimeError(
            f"cold start {round_index}: code-identity callback semantics failed\n{logs}"
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
    build_mode: str = "release",
    dobby_root: Path | None = None,
) -> None:
    if rounds <= 0:
        raise ValueError("rounds must be greater than zero")
    if timeout_seconds <= 0:
        raise ValueError("timeout must be greater than zero")
    build_mode = _normalize_flutter_mode(build_mode)

    if build:
        build_flutter_fixture(
            flutter=flutter, dobby_root=dobby_root, build_mode=build_mode
        )
    apk_path = flutter_fixture_apk_path(build_mode)
    if not apk_path.is_file():
        raise FileNotFoundError(f"missing Flutter {build_mode} fixture APK: {apk_path}")
    _assert_no_packaged_runtime_metadata(apk_path)

    serial = find_arm64_device(device)
    run(adb_cmd(["install", "-r", str(apk_path)], device=serial))

    results = [
        _validate_round(serial, index, timeout_seconds, build_mode)
        for index in range(1, rounds + 1)
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
