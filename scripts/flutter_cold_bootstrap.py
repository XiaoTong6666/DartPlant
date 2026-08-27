from __future__ import annotations

import os
import re
import shutil
import subprocess as sp
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path

from util import ROOT_DIR, adb_cmd, find_arm64_device, run


FIXTURE_DIR = ROOT_DIR / "tests" / "flutter_fixture"
APK_PATH = FIXTURE_DIR / "build" / "app" / "outputs" / "flutter-apk" / "app-release.apk"
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


def _build_fixture(flutter: str) -> None:
    run([flutter, "pub", "get"], cwd=FIXTURE_DIR, env=os.environ.copy())
    run(
        [
            flutter,
            "build",
            "apk",
            "--release",
            "--target-platform",
            "android-arm64",
        ],
        cwd=FIXTURE_DIR,
        env=os.environ.copy(),
    )
    if not APK_PATH.is_file():
        raise FileNotFoundError(f"Flutter release APK was not produced: {APK_PATH}")


def _assert_no_runtime_metadata() -> None:
    with zipfile.ZipFile(APK_PATH) as archive:
        entries = archive.namelist()
    forbidden = [
        entry
        for entry in entries
        if "dartplant" in entry.lower() and "metadata" in entry.lower()
    ]
    if forbidden:
        raise RuntimeError(
            "metadata-free fixture still packages DartPlant metadata: " + ", ".join(forbidden)
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
            and "DartPlant FunctionType semantic probe:" in latest
            and "DartPlant FunctionType named semantic probe:" in latest
            and "DartPlant bool semantic probe:" in latest
            and "DartPlant live VM startup probe:" in latest
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
) -> None:
    if rounds <= 0:
        raise ValueError("rounds must be greater than zero")
    if timeout_seconds <= 0:
        raise ValueError("timeout must be greater than zero")

    flutter_bin = _resolve_flutter(flutter) if build else ""
    if build:
        _build_fixture(flutter_bin)
    if not APK_PATH.is_file():
        raise FileNotFoundError(f"missing Flutter fixture APK: {APK_PATH}")
    _assert_no_runtime_metadata()

    serial = find_arm64_device(device)
    run(adb_cmd(["install", "-r", str(APK_PATH)], device=serial))

    results = [
        _validate_round(serial, index, timeout_seconds) for index in range(1, rounds + 1)
    ]
    sampled = [result.sampled for result in results]
    no_dart_pc = sum(result.dart_pc == 0 for result in results)
    print(
        "metadata-free cold bootstrap: "
        f"{len(results)}/{rounds} passed; "
        f"sampled min={min(sampled)} max={max(sampled)}; "
        f"validated={sum(result.validated for result in results)}; "
        f"dart_pc_zero_rounds={no_dart_pc}"
    )
