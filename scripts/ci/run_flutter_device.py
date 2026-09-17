#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import subprocess as sp
import time
from pathlib import Path

from common import ACTIVITY, ARM64_ABI, PACKAGE, RUNTIME_SCENARIOS, inspect_arm64_apk


def _adb(serial: str, *args: str) -> list[str]:
    return ["adb", "-s", serial, *args]


def _capture(command: list[str], *, check: bool = True, timeout: float = 30.0) -> str:
    result = sp.run(
        command,
        check=False,
        stdout=sp.PIPE,
        stderr=sp.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed with exit code {result.returncode}: {command}\n{result.stdout}"
        )
    return result.stdout


def _resolve_serial(requested: str | None) -> str:
    if requested:
        return requested
    output = _capture(["adb", "devices"])
    devices = [
        line.split()[0]
        for line in output.splitlines()[1:]
        if line.strip().endswith("\tdevice")
    ]
    # Local development machines commonly keep an adb-over-network device
    # connected while the translated ARM smoke starts an emulator. CI should
    # not accidentally select (or fail on) an unrelated developer device when
    # exactly one emulator is available. Keep the old strict behavior for
    # multiple real candidates, but prefer the Android emulator created by the
    # workflow when it is unambiguous.
    emulator_devices = [device for device in devices if device.startswith("emulator-")]
    if len(emulator_devices) == 1:
        return emulator_devices[0]
    if len(devices) != 1:
        raise RuntimeError(f"expected exactly one adb device, found {devices}")
    return devices[0]


def _dump_logcat(serial: str, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        text = _capture(_adb(serial, "logcat", "-d", "-v", "threadtime"), check=False)
    except Exception as error:  # Preserve runner metadata even when adb itself is gone.
        text = f"logcat collection failed: {error}\n"
    path.write_text(text, errors="replace")


def _clean_install(serial: str, apks: list[Path], *, timeout: float) -> str:
    """Install one fixture from a clean package state and return uninstall output."""
    # Real devices can reject uninstall with DELETE_FAILED_INTERNAL_ERROR while
    # the previous fixture process is still alive. A following install can then
    # race that old process and make the next matrix row execute the previous
    # Flutter/Dart image even though the APK on disk is correct. Stop the old
    # process first, then require both pid and package path to disappear before
    # installing the next family.
    _capture(
        _adb(serial, "shell", "am", "force-stop", PACKAGE),
        check=False,
        timeout=timeout,
    )
    stop_deadline = time.monotonic() + min(timeout, 10.0)
    stale_pid = ""
    while time.monotonic() < stop_deadline:
        stale_pid = _capture(
            _adb(serial, "shell", "pidof", PACKAGE),
            check=False,
            timeout=min(timeout, 20.0),
        ).strip()
        if not stale_pid:
            break
        time.sleep(0.1)
    if stale_pid:
        raise RuntimeError(
            f"failed to stop previous runtime before install: pid={stale_pid}"
        )

    uninstall_attempts: list[str] = []
    stale_package = ""
    for _ in range(3):
        uninstall = _capture(
            _adb(serial, "uninstall", PACKAGE),
            check=False,
            timeout=timeout,
        ).strip()
        uninstall_attempts.append(uninstall)

        # `adb uninstall` may return before vendor PackageManager bookkeeping
        # has fully drained. HyperOS in particular can otherwise race the
        # immediately following install session. These commands are best-effort
        # so older Android shells remain usable.
        _capture(
            _adb(
                serial,
                "shell",
                "cmd",
                "package",
                "wait-for-handler",
                "--timeout",
                "5000",
            ),
            check=False,
            timeout=min(timeout, 10.0),
        )
        _capture(
            _adb(
                serial,
                "shell",
                "cmd",
                "package",
                "wait-for-background-handler",
                "--timeout",
                "5000",
            ),
            check=False,
            timeout=min(timeout, 10.0),
        )

        removal_deadline = time.monotonic() + min(timeout, 10.0)
        while time.monotonic() < removal_deadline:
            stale_package = _capture(
                _adb(serial, "shell", "pm", "path", PACKAGE),
                check=False,
                timeout=min(timeout, 20.0),
            ).strip()
            stale_pid = _capture(
                _adb(serial, "shell", "pidof", PACKAGE),
                check=False,
                timeout=min(timeout, 20.0),
            ).strip()
            if not stale_package and not stale_pid:
                break
            time.sleep(0.1)
        if not stale_package and not stale_pid:
            break

        _capture(
            _adb(serial, "shell", "am", "force-stop", PACKAGE),
            check=False,
            timeout=timeout,
        )
    else:
        raise RuntimeError(
            "failed to establish a clean package state before runtime: "
            f"path={stale_package!r} pid={stale_pid!r} "
            f"uninstall_attempts={uninstall_attempts!r}"
        )

    resolved_apks = [str(apk.resolve()) for apk in apks]
    install_args = (
        ["install", resolved_apks[0]]
        if len(resolved_apks) == 1
        else ["install-multiple", "-r", *resolved_apks]
    )
    _capture(_adb(serial, *install_args), timeout=timeout)
    return " | ".join(uninstall_attempts)


def _launch_command(serial: str, test: str) -> list[str]:
    # Keep runtime fixtures in fullscreen. Some vendor Android builds may
    # restore a freshly installed activity into freeform/multi-window and then
    # relaunch it once the final window configuration arrives. That starts a
    # second Dart runtime in the same process and contaminates cold lifecycle
    # proofs (notably deferred image-set state).
    return _adb(
        serial,
        "shell",
        "am",
        "start",
        "-W",
        "--windowingMode",
        "1",
        "-n",
        ACTIVITY,
        "--es",
        "dartplant_test",
        test,
    )


def _write_metadata(path: Path, metadata: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run one prebuilt DartPlant Flutter APK and capture raw logcat"
    )
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--deferred-apk", type=Path)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--serial")
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--test", default="all", choices=("all", *RUNTIME_SCENARIOS))
    parser.add_argument(
        "--runtime-tier", default="native", choices=("native", "translated-smoke")
    )
    args = parser.parse_args()
    if args.timeout <= 0:
        raise ValueError("--timeout must be greater than zero")

    metadata: dict[str, object] = {
        "package": PACKAGE,
        "runner_state": "starting",
        "test": args.test,
        "runtime_tier": args.runtime_tier,
    }
    serial: str | None = None
    try:
        metadata["apk"] = inspect_arm64_apk(args.apk, args.deferred_apk)
        serial = _resolve_serial(args.serial)
        metadata["serial"] = serial
        adb_wait_timeout = 120.0 if args.runtime_tier == "native" else 60.0
        install_timeout = 600.0 if args.runtime_tier == "native" else 120.0
        launch_timeout = 300.0 if args.runtime_tier == "native" else 60.0

        _capture(_adb(serial, "wait-for-device"), timeout=adb_wait_timeout)
        guest_abi = _capture(
            _adb(serial, "shell", "getprop", "ro.product.cpu.abi")
        ).strip()
        guest_abilist = _capture(
            _adb(serial, "shell", "getprop", "ro.product.cpu.abilist")
        ).strip()
        guest_uname = _capture(_adb(serial, "shell", "uname", "-m")).strip()
        metadata.update(
            {
                "guest_abi": guest_abi,
                "guest_abilist": guest_abilist,
                "guest_uname_m": guest_uname,
            }
        )
        if args.runtime_tier == "native" and (
            guest_abi != ARM64_ABI or guest_uname != "aarch64"
        ):
            raise RuntimeError(
                "authoritative runtime requires a native ARM64 Android guest: "
                f"abi={guest_abi!r} uname={guest_uname!r}"
            )

        # Each Flutter family is built in an isolated GitHub job and may be
        # signed by a different ephemeral Android debug/release key. Reusing
        # the same package with `adb install -r` therefore violates Android's
        # update-signature check. Runtime families must also not inherit app
        # data or extracted native libraries from the previous ABI proof.
        install_apks = [args.apk]
        if args.deferred_apk is not None:
            install_apks.append(args.deferred_apk)
        metadata["uninstall"] = _clean_install(
            serial, install_apks, timeout=install_timeout
        )

        _capture(_adb(serial, "logcat", "-G", "16M"), check=False)
        _capture(_adb(serial, "logcat", "-c"), check=False)
        _capture(_adb(serial, "shell", "am", "force-stop", PACKAGE), check=False)
        launch = _capture(_launch_command(serial, args.test), timeout=launch_timeout)
        metadata["launch"] = launch.strip()

        pid = ""
        pid_deadline = time.monotonic() + 10.0
        while time.monotonic() < pid_deadline:
            pid = _capture(_adb(serial, "shell", "pidof", PACKAGE), check=False).strip()
            if pid:
                break
            time.sleep(0.1)
        metadata["pid"] = pid

        deadline = time.monotonic() + args.timeout
        terminal_seen = False
        process_exited = False
        while time.monotonic() < deadline:
            current = _capture(
                _adb(serial, "logcat", "-d", "-v", "brief"),
                check=False,
                timeout=20.0,
            )
            if '"event":"suite"' in current and (
                '"state":"pass"' in current or '"state":"fail"' in current
            ):
                terminal_seen = True
                break
            if pid:
                alive = _capture(
                    _adb(serial, "shell", "pidof", PACKAGE), check=False
                ).strip()
                if not alive:
                    process_exited = True
                    break
            time.sleep(0.25)

        if terminal_seen:
            metadata["runner_state"] = "suite_seen"
        elif process_exited:
            metadata["runner_state"] = "process_exited"
        else:
            metadata["runner_state"] = "timeout"
        time.sleep(0.5)
    except Exception as error:
        metadata["runner_state"] = "error"
        metadata["runner_error"] = str(error)
    finally:
        if serial is not None:
            _dump_logcat(serial, args.log)
            try:
                _capture(_adb(serial, "shell", "am", "force-stop", PACKAGE), check=False)
            except Exception as cleanup_error:
                metadata["cleanup_error"] = str(cleanup_error)
        else:
            args.log.parent.mkdir(parents=True, exist_ok=True)
            args.log.write_text("device resolution failed before logcat capture\n")
        _write_metadata(args.metadata, metadata)

    print(json.dumps(metadata, indent=2, sort_keys=True))
    # Runtime correctness is gated by analyze_logcat.py so reports/logs are
    # still produced for timeout, process death, and provenance failures.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
