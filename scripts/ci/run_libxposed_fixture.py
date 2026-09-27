#!/usr/bin/env python3
"""Run the test-only API 102 module against DartPlant's external Flutter fixture.

Translated x86_64 Vector provides Java lifecycle injection; its native loader
cannot see ARM64 guest dlopen, so the guest physical backend is strict Dobby.
Native ARM64 hosts must instead prove a real Native API v2 initialization.
"""

import argparse
import json
import subprocess
import time
from pathlib import Path

from common import COMMON_RUNTIME_SCENARIOS, RUNTIME_SCENARIOS

TARGET = "dev.dartplant.dartplant_fixture"
MODULE = "dev.dartplant.integration"
REQUIRED_DOBBY_SCENARIOS = (*COMMON_RUNTIME_SCENARIOS, *RUNTIME_SCENARIOS)


def adb(serial: str, *args: str, check: bool = True) -> str:
    command = ["adb", "-s", serial, *args]
    result = subprocess.run(command, text=True, capture_output=True)
    if check and result.returncode:
        raise RuntimeError(
            f"{' '.join(command)} returned {result.returncode}: {result.stderr}"
        )
    return result.stdout + result.stderr


def cli(serial: str, *args: str) -> str:
    command = "/data/adb/modules/zygisk_vector/cli " + " ".join(args)
    return adb(serial, "shell", "su", "-c", command)


def extract_scenario(logs: str, name: str = "external_module") -> dict | None:
    events = []
    for line in logs.splitlines():
        marker = "DARTPLANT_CI "
        if marker not in line:
            continue
        try:
            event = json.loads(line.split(marker, 1)[1])
        except json.JSONDecodeError:
            continue
        if event.get("event") == "scenario" and event.get("name") == name:
            events.append(event)
    return events[-1] if events else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--module", type=Path)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--configure-vector", action="store_true")
    parser.add_argument("--expect-backend", choices=("translated-dobby", "native-v2",
                                                    "native-strict-dobby"),
                        required=True)
    parser.add_argument(
        "--expect-family-mismatch",
        action="store_true",
        help="Require the fixed adapter to reject an incompatible Flutter artifact before hooking",
    )
    parser.add_argument(
        "--expect-artifact-mismatch",
        action="store_true",
        help="Require exact AOT compiler evidence to reject a same-family stale app build ID before publishing any hooks",
    )
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument(
        "--require-dobby-parity",
        action="store_true",
        help="Fail unless all embedded Dobby scenario contracts have independent external evidence",
    )
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    if args.module:
        print(adb(args.serial, "install", "-r", str(args.module)))
    if args.fixture:
        deferred_fixture = args.fixture.with_name(
            args.fixture.name.replace("app-", "deferred_probe-", 1)
        )
        if deferred_fixture != args.fixture and deferred_fixture.is_file():
            print(
                adb(
                    args.serial,
                    "install-multiple",
                    "-r",
                    str(args.fixture),
                    str(deferred_fixture),
                )
            )
        else:
            print(adb(args.serial, "install", "-r", str(args.fixture)))
    if args.configure_vector:
        print(cli(args.serial, "modules", "enable", MODULE))
        print(cli(args.serial, "scope", "set", MODULE, TARGET))

    adb(args.serial, "shell", "am", "force-stop", TARGET)
    adb(args.serial, "logcat", "-c")
    print(adb(args.serial, "shell", "am", "start", "-n", TARGET + "/.MainActivity"))
    deadline = time.monotonic() + args.timeout
    logs = ""
    scenario = None
    while time.monotonic() < deadline:
        logs = adb(args.serial, "logcat", "-d")
        scenario = extract_scenario(logs)
        if scenario is not None:
            break
        time.sleep(1.0)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text(logs)
    backend_markers = {
        "translated-dobby": "event=translated_backend, mode=arm64_strict_dobby, status=0",
        "native-v2": "event=native_v2_selftest, state=pass",
        "native-strict-dobby":
            "event=native_strict_backend, mode=arm64_strict_dobby, "
            "native_v2_selftest=fail, status=0",
    }
    backend_marker = backend_markers[args.expect_backend]
    null_semantics = extract_scenario(logs, "external_null_semantics")
    bool_semantics = extract_scenario(logs, "external_bool_semantics")
    gc_pressure = extract_scenario(logs, "external_gc_pressure")
    generic_closure = extract_scenario(logs, "external_generic_closure")
    generic_gc = extract_scenario(logs, "external_generic_gc")
    closure_receiver = extract_scenario(logs, "external_closure_receiver")
    ordinary_aot = extract_scenario(logs, "external_ordinary_aot")
    late_shared = extract_scenario(logs, "external_late_shared")
    p6_scenario = extract_scenario(logs, "external_p6_abi")
    exception_scenario = extract_scenario(logs, "external_exception")
    object_root_scenario = extract_scenario(logs, "external_object_root_gc")
    callback_drain = extract_scenario(logs, "external_callback_drain")
    deferred_lifecycle = extract_scenario(logs, "external_deferred_lifecycle")
    changed_slotless_verified = (
        "event=changed_slotless_reject, state=pass" in logs
        and "event=deferred_lifecycle, state=pass" in logs
    )
    semantic_native = (
        "event=semantic_probe, state=pass, null_enter=2, null_leave=2, "
        "null_override=1, null_failures=0, bool_true=1, bool_false=1, "
        "bool_failures=0" in logs
    )
    generic_closure_verified = (
        generic_closure is not None
        and generic_closure.get("state") == "pass"
        and "event=typeargs_callback, state=pass, require_relocation=0" in logs
        and "event=typeargs_probe, state=pass, enter=1, failures=0, require_relocation=0"
        in logs
    )
    generic_gc_verified = (
        generic_gc is not None
        and generic_gc.get("state") == "pass"
        and "event=typeargs_callback, state=pass, require_relocation=1" in logs
        and "parameter_relocated=1" in logs
        and "event=typeargs_probe, state=pass, enter=1, failures=0, require_relocation=1"
        in logs
    )
    arguments_descriptor_verified = (
        generic_closure_verified
        and generic_gc_verified
        and logs.count("type_args=1, named=2") >= 2
    )
    p6_verified = (
        p6_scenario is not None
        and p6_scenario.get("state") == "pass"
        and p6_scenario.get("install") == 0
        and p6_scenario.get("probe") == 1
        and p6_scenario.get("int64_before") == 1000000000000000007
        and p6_scenario.get("int64_after") == 3000000010000000113
        and p6_scenario.get("stack_before") == 108.0
        and p6_scenario.get("stack_after") == 1146.0
        and p6_scenario.get("odd_before") == 91.0
        and p6_scenario.get("odd_after") == 217.0
        and p6_scenario.get("throw_path") == 2
        and p6_scenario.get("throw_before") == 108.0
        and p6_scenario.get("throw_after") == 125.0
        and p6_scenario.get("forced_before") == 34
        and p6_scenario.get("forced_after") == 65
        and p6_scenario.get("pair_before_first") == 21
        and p6_scenario.get("pair_before_second") == 22
        and p6_scenario.get("pair_after_first") == 32
        and p6_scenario.get("pair_after_second") == 31
        and p6_scenario.get("object_pair_first") is True
        and p6_scenario.get("object_pair_second") is True
        and "event=p6_probe, state=pass" in logs
        and "exception_object=1" in logs
        and "moving_pair=1" in logs
    )
    initialization_verified = (
        scenario is not None
        and scenario.get("bootstrap") == 1
        and backend_marker in logs
        and "event=module_loaded, native_ready=true" in logs
        and "event=owner_entry, state=hooked" in logs
    )
    live_vm_startup_verified = (
        scenario is not None
        and scenario.get("dart_result") == 115
        and "event=result_rewrite, state=pass, before=5, after=115, proof=result_only" in logs
    )
    closure_verified = (
        closure_receiver is not None
        and closure_receiver.get("state") == "pass"
        and closure_receiver.get("value") == 78
        and closure_receiver.get("probe") == 1
        and "event=closure_probe, state=pass, enter=1, failures=0, active=1" in logs
    )
    ordinary_verified = (
        ordinary_aot is not None
        and ordinary_aot.get("state") == "pass"
        and ordinary_aot.get("install") == 0
        and ordinary_aot.get("direct") == 16.125
        and ordinary_aot.get("after_shared") == 6.25
        and ordinary_aot.get("probe") == 1
        and "event=ordinary_probe, state=pass, enter=1, leave=1, observer=1, failures=0, cleanup=1"
        in logs
    )
    exception_verified_for_runtime = (
        exception_scenario is not None
        and exception_scenario.get("state") == "pass"
        and exception_scenario.get("caught") == 2
        and exception_scenario.get("native") == 1
        and p6_verified
        and p6_scenario.get("throw_path") == 2
        and "event=exception_probe, state=pass, enter=1, leave=0, unwind=1, "
        "phase_safe=1, object_api_available=0, hook_idle=1" in logs
    )
    independent_parity = {
        "initialization": initialization_verified,
        "null_semantics": null_semantics is not None
        and null_semantics.get("state") == "pass" and semantic_native,
        "bool_semantics": bool_semantics is not None
        and bool_semantics.get("state") == "pass" and semantic_native,
        "arguments_descriptor": arguments_descriptor_verified,
        "generic_closure": generic_closure_verified,
        "generic_gc": generic_gc_verified,
        "closure_receiver": closure_verified,
        "closure": closure_verified,
        "advanced_ordinary": ordinary_verified,
        "ordinary_aot": ordinary_verified,
        "live_vm_startup": live_vm_startup_verified,
        "exception": exception_verified_for_runtime,
        "transition": "event=transition, state=pass" in logs,
        "artifact_revalidate": "event=artifact_revalidate, state=pass" in logs,
        "late_shared": late_shared is not None
        and late_shared.get("state") == "pass"
        and late_shared.get("transition") == 1
        and late_shared.get("typed_after_shared") == 6.25
        and "event=ordinary_mark_shared, state=pass" in logs,
        "p6_abi": p6_verified,
        "deferred_lifecycle": deferred_lifecycle is not None
        and deferred_lifecycle.get("state") == "pass"
        and deferred_lifecycle.get("value") == 42
        and deferred_lifecycle.get("post_refresh_hook") == 115
        and "event=deferred_lifecycle, state=pass" in logs,
        "changed_slotless_reject": changed_slotless_verified,
    }
    parity_covered = [
        name for name in REQUIRED_DOBBY_SCENARIOS if independent_parity.get(name, False)
    ]
    parity_missing = [
        name for name in REQUIRED_DOBBY_SCENARIOS if name not in parity_covered
    ]
    root_pass = (
        object_root_scenario is not None
        and object_root_scenario.get("state") == "pass"
        and object_root_scenario.get("strong_root_alive") is True
        and "event=object_root_probe, state=pass, leave=2, failures=0, alive=1" in logs
    )
    root_relocated = root_pass and object_root_scenario.get("root_address_changed") is True
    evidence = {
        "backend": args.expect_backend,
        "scenario": scenario,
        "owner_lifecycle": extract_scenario(logs, "external_owner_lifecycle"),
        "null_semantics": null_semantics,
        "bool_semantics": bool_semantics,
        "gc_pressure": gc_pressure,
        "generic_closure": generic_closure,
        "generic_gc": generic_gc,
        "closure_receiver": closure_receiver,
        "ordinary_aot": ordinary_aot,
        "late_shared": late_shared,
        "p6_abi": p6_scenario,
        "exception": exception_scenario,
        "object_root_gc": object_root_scenario,
        "callback_drain": callback_drain,
        "deferred_lifecycle": deferred_lifecycle,
        "object_root_relocated": root_relocated,
        "independent_external_probes": {
            "smi_result_rewrite": scenario is not None
            and scenario.get("dart_result") == 115
            and scenario.get("unhooked_result") == 5
            and scenario.get("rebound_result") == 115,
            "null_semantics": independent_parity["null_semantics"],
            "bool_semantics": independent_parity["bool_semantics"],
            "ordinary_result_object_root": root_pass,
            "ordinary_result_root_relocated": root_relocated,
            "gc_allocation_pressure": gc_pressure is not None
            and gc_pressure.get("state") == "pass",
            "nonlocal_exception_unwind": exception_scenario is not None
            and exception_scenario.get("state") == "pass",
            "typearguments_generated_root_relocation": generic_gc_verified,
            "p6_full_corpus": p6_verified,
        },
        "dobby_parity_covered": parity_covered,
        "dobby_parity_missing": parity_missing,
        "dobby_parity_complete": not parity_missing,
        "parity_limitations": [
            "external owner-thread argument ABI remains unverified",
            "ordinary strong-result object root relocation and TypeArguments generated-root relocation have separate evidence",
            "basic external exception unwind and P6 exception object access are ported; bridge self-unhook lifetime is not",
            "external forced-closure/deferred suites are not yet ported",
        ],
        "backend_verified": backend_marker in logs,
        "physical_hooked": logs.count("event=owner_entry, state=hooked") >= 2,
        "retirement_verified": "event=owner_retire, state=pass" in logs,
        "loader_callback_drain_verified": (
            callback_drain is not None
            and callback_drain.get("state") == "pass"
            and callback_drain.get("runtime_retired") == 1
            and callback_drain.get("loader_callback_drained") == 1
            and "event=loader_callback_drain, state=pass, in_flight=0" in logs
        ),
        "rewrite_verified": (
            "event=result_rewrite, state=pass, before=5, after=115, proof=result_only" in logs
            and "rewrite_failures=0" in logs
        ),
        "semantic_verified": semantic_native,
        "exception_verified": (
            "event=exception_probe, state=pass, enter=1, leave=0, unwind=1, "
            "phase_safe=1, object_api_available=0, hook_idle=1" in logs
        ),
        "object_root_verified": (
            "event=object_root_probe, state=pass, leave=2, failures=0, alive=1" in logs
        ),
        "mapping_control_verified": (
            "event=physical_mapping_control, state=pass" in logs
            and "mapping_removed=1, same_va=1, different_inode=1, owner_reconciled=1" in logs
        ),
        "java_module_loaded": "event=module_loaded, native_ready=true" in logs,
        "callback_abi_verified": (
            "event=package_ready, hook=installed, bootstrap=registered, callback_abi=2"
            in logs
        ),
        "process_alive": TARGET in adb(args.serial, "shell", "ps", "-A", check=False),
    }
    if args.expect_family_mismatch and args.expect_artifact_mismatch:
        parser.error("choose exactly one expected rejection kind")
    if args.expect_family_mismatch:
        evidence["expected_rejection"] = (
            "event=owner_entry, state=fail, stage=fixed_descriptor" in logs
            and not evidence["physical_hooked"]
            and not evidence["retirement_verified"]
        )
        evidence["passed"] = (
            scenario is not None
            and scenario.get("state") == "fail"
            and scenario.get("bootstrap") == 0
            and scenario.get("java_value") == 107
            and evidence["backend_verified"]
            and evidence["java_module_loaded"]
            and evidence["callback_abi_verified"]
            and evidence["process_alive"]
            and evidence["expected_rejection"]
        )
    elif args.expect_artifact_mismatch:
        evidence["expected_rejection"] = (
            "event=artifact_preflight, state=reject, before_hook_publish=1" in logs
            and "event=owner_entry, state=fail, stage=external_root_preflight, status=8" in logs
            and "event=owner_entry, state=hooked" not in logs
            and "event=result_rewrite, state=pass" not in logs
        )
        evidence["passed"] = (
            scenario is not None
            and scenario.get("state") == "fail"
            and scenario.get("bootstrap") == 0
            and scenario.get("java_value") == 107
            and evidence["backend_verified"]
            and evidence["java_module_loaded"]
            and evidence["callback_abi_verified"]
            and evidence["process_alive"]
            and evidence["expected_rejection"]
        )
    else:
        evidence["passed"] = (
            scenario is not None
            and scenario.get("state") == "pass"
            and scenario.get("dart_result") == 115
            and scenario.get("unhooked_result") == 5
            and scenario.get("rebound_result") == 115
            and evidence["null_semantics"] is not None
            and evidence["null_semantics"].get("state") == "pass"
            and evidence["bool_semantics"] is not None
            and evidence["bool_semantics"].get("state") == "pass"
            and evidence["gc_pressure"] is not None
            and evidence["gc_pressure"].get("state") == "pass"
            and evidence["gc_pressure"].get("before") == 115
            and evidence["gc_pressure"].get("after") == 115
            and evidence["gc_pressure"].get("object_root_relocation_proven") is False
            and generic_closure_verified
            and generic_gc_verified
            and arguments_descriptor_verified
            and p6_verified
            and exception_scenario is not None
            and exception_scenario.get("state") == "pass"
            and exception_scenario.get("caught") == 2
            and evidence["exception_verified"]
            and root_pass
            and evidence["object_root_verified"]
            and evidence["owner_lifecycle"] is not None
            and evidence["owner_lifecycle"].get("state") == "pass"
            and evidence["owner_lifecycle"].get("b_value") == 5
            and evidence["owner_lifecycle"].get("b2_value") == 5
            and evidence["owner_lifecycle"].get("a_after_b") == 115
            and evidence["owner_lifecycle"].get("a_after_b2") == 115
            and evidence["backend_verified"]
            and evidence["physical_hooked"]
            and evidence["retirement_verified"]
            and evidence["loader_callback_drain_verified"]
            and evidence["rewrite_verified"]
            and evidence["semantic_verified"]
            and evidence["mapping_control_verified"]
            and evidence["java_module_loaded"]
            and evidence["callback_abi_verified"]
            and evidence["process_alive"]
        )
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(evidence, sort_keys=True, indent=2) + "\n")
    print(json.dumps(evidence, sort_keys=True, indent=2))
    return 0 if evidence["passed"] and (
        not args.require_dobby_parity or evidence["dobby_parity_complete"]
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
