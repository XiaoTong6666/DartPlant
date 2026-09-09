#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

try:
    from .capability_registry import mask as capability_mask
    from .capability_registry import required_event_capabilities
    from .common import RUNTIME_SCENARIOS
except ImportError:
    from capability_registry import mask as capability_mask
    from capability_registry import required_event_capabilities
    from common import RUNTIME_SCENARIOS


CI_PREFIX = "DARTPLANT_CI "
LEGACY_PROOFS = {
    "Source structural proof": (
        "result=pass stage=complete",
        "dart_core=1",
    ),
    "Flutter profile gate": ("DartPlant Flutter VM adapter profile gate: 1 count=3",),
    "FunctionType proofs": (
        "DartPlant FunctionType semantic probe: 1",
        "DartPlant FunctionType named semantic probe: 1",
    ),
    "Code entry family": ("entry-family instrumentedAdd mask=0xf",),
    "TypeArguments moving GC": (
        "parameter_relocated=1",
        "TypeArguments proof native summary enter=1 failures=0",
        "DartPlant app TypeArguments proof: 1 source=adb native=1",
        "require_relocation=1",
        "result_ok=1",
    ),
    "Exception unwind lifetime": (
        "exception callback phase-safe access exception=1 stacktrace=1 argument_rejected=1 "
        "raw_gp_rejected=1",
        "exception bridge lifetime probe enter=1 leave=0 unhook=1 idle=1 inactive=1 "
        "failures=0 shutdown=1 passed=1",
    ),
    "Live VM model": ("runtime live-vm lookup addInt ok", "model_ok=1"),
}


@dataclass(frozen=True)
class Check:
    name: str
    passed: bool
    detail: str


@dataclass(frozen=True)
class Analysis:
    flutter: str
    dart: str
    test: str
    runtime_tier: str
    checks: tuple[Check, ...]
    malformed_events: tuple[str, ...]
    crash_hits: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return all(check.passed for check in self.checks)


def _hex(value: object) -> int:
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        return int(value, 0)
    raise ValueError(f"not an integer/hex value: {value!r}")


def _int(value: object) -> int:
    if isinstance(value, bool):
        raise ValueError("boolean is not an integer here")
    return int(value)


def parse_events(log_text: str) -> tuple[list[dict[str, object]], list[str]]:
    events: list[dict[str, object]] = []
    malformed: list[str] = []
    for line in log_text.splitlines():
        position = line.find(CI_PREFIX)
        if position < 0:
            continue
        payload = line[position + len(CI_PREFIX) :].strip()
        try:
            event = json.loads(payload)
            if not isinstance(event, dict) or not isinstance(event.get("event"), str):
                raise ValueError("event payload must be a JSON object with an event name")
        except (json.JSONDecodeError, ValueError) as error:
            malformed.append(f"{error}: {payload}")
            continue
        events.append(event)
    return events, malformed


def _event_group(
    events: Iterable[dict[str, object]], event_name: str
) -> list[dict[str, object]]:
    return [event for event in events if event.get("event") == event_name]


def _capability_event_detail(event: dict[str, object]) -> str:
    return (
        f"capability={event.get('capability')} state={event.get('state')} "
        f"source_rows={event.get('source_rows')} "
        f"root_compatible={event.get('root_compatible')} "
        f"relational_passed={event.get('relational_passed')} "
        f"distinct_keys={event.get('distinct_keys')} "
        f"selected_rows={event.get('selected_rows')} "
        f"artifact_generation={event.get('artifact_generation')} "
        f"isolate_generation={event.get('isolate_generation')}"
    )


def _check_runtime(
    events: list[dict[str, object]], expected_flutter: str, expected_dart: str
) -> Check:
    runtime = _event_group(events, "runtime")
    matching = [
        event
        for event in runtime
        if event.get("flutter") == expected_flutter
        and event.get("dart") == expected_dart
        and event.get("dart_runtime") == expected_dart
        and event.get("abi") == "arm64-v8a"
        and event.get("dart_ffi_abi") == "android_arm64"
    ]
    return Check(
        "Runtime provenance",
        bool(matching),
        f"events={len(runtime)} matching={len(matching)} "
        f"expected={expected_flutter}/{expected_dart}/arm64-v8a/android_arm64",
    )


def _check_device(metadata: dict[str, object], runtime_tier: str) -> Check:
    apk = metadata.get("apk")
    apk_ok = isinstance(apk, dict) and apk.get("abi") == "arm64-v8a" and apk.get(
        "elf_machine"
    ) == "AArch64"
    if runtime_tier == "native":
        guest_ok = metadata.get("guest_abi") == "arm64-v8a" and metadata.get(
            "guest_uname_m"
        ) == "aarch64"
        label = "Native ARM64 provenance"
    else:
        guest_ok = metadata.get("guest_abi") in {"x86_64", "arm64-v8a"} and metadata.get(
            "guest_uname_m"
        ) in {"x86_64", "aarch64"}
        label = "Translated smoke provenance"
    passed = metadata.get("runner_state") == "suite_seen" and guest_ok and apk_ok
    runner_error = str(metadata.get("runner_error", "")).replace("\n", " ").strip()
    if len(runner_error) > 300:
        runner_error = runner_error[:297] + "..."
    error_detail = f" runner_error={runner_error}" if runner_error else ""
    return Check(
        label,
        passed,
        "runner_state={} guest={}/{} apk={}{}".format(
            metadata.get("runner_state"),
            metadata.get("guest_abi"),
            metadata.get("guest_uname_m"),
            "AArch64" if apk_ok else "invalid",
            error_detail,
        ),
    )


def _check_core_binding(events: list[dict[str, object]]) -> Check:
    bindings = _event_group(events, "core_binding")
    valid = []
    for event in bindings:
        try:
            valid.append(
                event.get("state") == "verified"
                and _int(event.get("candidate_count", 0)) >= 1
                and _int(event.get("passing_count", 0)) >= 1
                and _int(event.get("distinct_abis", 0)) == 1
            )
        except (TypeError, ValueError):
            valid.append(False)
    passed = bool(bindings) and all(valid)
    return Check(
        "Unique core ABI binding",
        passed,
        f"bindings={len(bindings)} verified={sum(valid)} distinct ABI must equal 1",
    )


def _select_lifecycle(
    events: list[dict[str, object]],
) -> tuple[Check, int | None, int | None]:
    lifecycle = _event_group(events, "artifact_lifecycle")
    valid_generations: list[int] = []
    valid_isolate_generations: list[int] = []
    invalid_details: list[str] = []
    for event in lifecycle:
        try:
            capabilities = _hex(event["capabilities"])
            required = _hex(event["required"])
            verified_before = _hex(event["verified_before"])
            verified_invalidated = _hex(event["verified_invalidated"])
            verified_revalidated = _hex(event["verified_revalidated"])
            failed_before = _hex(event["failed_before"])
            failed_revalidated = _hex(event["failed_revalidated"])
            expected_verified = capability_mask(verified_after_create=True)
            generation_before = _int(event["generation_before"])
            generation_quiesced = _int(event["generation_quiesced"])
            generation_invalidated = _int(event["generation_invalidated"])
            generation_revalidated = _int(event["generation_revalidated"])
            isolate_generation = _int(event["isolate_generation"])
            relational = (
                event.get("state") == "pass"
                and required == capability_mask(cold_required=True)
                and (capabilities & required) == required
                and verified_before == expected_verified
                and verified_revalidated == expected_verified
                and verified_invalidated == 0
                and failed_before == 0
                and failed_revalidated == 0
                and generation_before > 0
                and generation_quiesced == generation_before
                and generation_invalidated == generation_before + 1
                and generation_revalidated == generation_invalidated
                and isolate_generation > 0
                and _int(event["quiesce_status"]) == 0
                and _int(event["retire_status"]) == 0
                and _int(event["revalidation_status"]) == 0
            )
            if relational:
                valid_generations.append(generation_revalidated)
                valid_isolate_generations.append(isolate_generation)
            else:
                invalid_details.append(json.dumps(event, sort_keys=True))
        except (KeyError, TypeError, ValueError) as error:
            invalid_details.append(f"{error}: {event}")
    passed = bool(lifecycle) and len(valid_generations) == len(lifecycle)
    detail = (
        f"events={len(lifecycle)} valid={len(valid_generations)} "
        f"revalidated_generation={valid_generations[-1] if valid_generations else 'none'}"
    )
    if invalid_details:
        detail += f" invalid={invalid_details[0]}"
    return (
        Check("Artifact lifecycle", passed, detail),
        valid_generations[-1] if valid_generations else None,
        valid_isolate_generations[-1] if valid_isolate_generations else None,
    )


def _check_capabilities(
    events: list[dict[str, object]], generation: int | None, isolate_generation: int | None
) -> list[Check]:
    capability_events = _event_group(events, "capability")
    failure_states = {
        "predicate_failed",
        "ambiguous",
        "generation_stale",
        "dependency_mismatch",
    }
    failed_events = [event for event in capability_events if event.get("state") in failure_states]
    checks = [
        Check(
            "Capability failures",
            not failed_events,
            f"failed_events={len(failed_events)}"
            + (f" first={_capability_event_detail(failed_events[0])}" if failed_events else ""),
        )
    ]
    for capability in required_event_capabilities():
        named = [
            event
            for event in capability_events
            if event.get("capability") == capability.diagnostic_name
        ]
        generation_matches = []
        for event in named:
            try:
                selected_rows = event.get("selected_rows")
                relational_passed = _int(event.get("relational_passed", -1))
                generation_matches.append(
                    _int(event.get("schema_version", 0)) >= 2
                    and event.get("state") == "verified"
                    and _hex(event.get("capability_bit", "0x0")) == capability.bit
                    and _hex(event.get("failed", "0x1")) == 0
                    and _int(event.get("source_rows", 0)) >= 1
                    and _int(event.get("root_compatible", 0)) >= relational_passed >= 1
                    and _int(event.get("distinct_keys", 0)) == 1
                    and isinstance(selected_rows, list)
                    and len(selected_rows) == relational_passed
                    and generation is not None
                    and _int(event.get("artifact_generation", -1)) == generation
                    and isolate_generation is not None
                    and _int(event.get("isolate_generation", -1)) == isolate_generation
                )
            except (TypeError, ValueError):
                generation_matches.append(False)
        checks.append(
            Check(
                f"Capability {capability.diagnostic_name}",
                any(generation_matches),
                f"events={len(named)} artifact_generation={generation} "
                f"isolate_generation={isolate_generation} matching={sum(generation_matches)}"
                + (f" last={_capability_event_detail(named[-1])}" if named else ""),
            )
        )
    return checks


def _check_scenarios(
    events: list[dict[str, object]], expected_test: str
) -> list[Check]:
    scenarios = _event_group(events, "scenario")
    checks: list[Check] = []
    required = RUNTIME_SCENARIOS if expected_test == "all" else (expected_test,)
    for name in required:
        named = [event for event in scenarios if event.get("name") == name]
        checks.append(
            Check(
                f"Scenario {name}",
                bool(named) and all(event.get("state") == "pass" for event in named),
                f"events={len(named)} states={[event.get('state') for event in named]}",
            )
        )
    suites = _event_group(events, "suite")
    checks.append(
        Check(
            "Suite completion",
            any(
                event.get("state") == "pass" and event.get("test") == expected_test
                for event in suites
            ),
            f"events={len(suites)}",
        )
    )
    return checks


def _check_generic_gc(events: list[dict[str, object]]) -> Check:
    gc_events = [
        event
        for event in _event_group(events, "type_arguments")
        if event.get("require_relocation") is True
    ]
    valid: list[tuple[str, str, int]] = []
    for event in gc_events:
        try:
            before = str(event["parameter_before"])
            after = str(event["parameter_after"])
            calls = _int(event["dart_api_calls"])
            if (
                event.get("state") == "pass"
                and event.get("parameter_relocated") is True
                and before != after
                and calls > 0
            ):
                valid.append((before, after, calls))
        except (KeyError, TypeError, ValueError):
            continue
    detail = "missing structured moving-GC proof"
    if valid:
        before, after, calls = valid[-1]
        detail = f"parameter {before} -> {after}; dart_api_calls={calls}; value preserved by scenario"
    return Check("Generic closure GC relocation", bool(valid), detail)


def _check_legacy_proofs(log_text: str, expected_test: str) -> list[Check]:
    checks: list[Check] = []
    for name, markers in LEGACY_PROOFS.items():
        if name == "TypeArguments moving GC" and expected_test not in {"all", "generic_gc"}:
            continue
        missing = [marker for marker in markers if marker not in log_text]
        checks.append(
            Check(
                name,
                not missing,
                "all required markers present" if not missing else f"missing={missing}",
            )
        )
    return checks


def _check_code_identity_semantics(log_text: str) -> Check:
    producer_lines = [
        line
        for line in log_text.splitlines()
        if "producer code-identity policy verified mode=" in line
    ]
    probe_lines = [
        line for line in log_text.splitlines() if "instrumentedAdd probe mode=" in line
    ]
    if not producer_lines or not probe_lines:
        return Check(
            "Code identity semantics",
            False,
            "missing producer code-identity policy or instrumentedAdd probe evidence",
        )

    producer = producer_lines[-1]
    probe = probe_lines[-1]
    common_probe = (
        "enter=5",
        "leave=5",
        "live_ok=5",
        "live_failed=0",
        "lookup_ok=1",
        "model_ok=1",
        "policy_ok=1",
        "result=115",
        "expected=115",
    )
    if "mode=no-dedup-unique" in producer:
        producer_markers = (
            "instrumented_aliases=1",
            "add_int_aliases=1",
        )
        probe_markers = common_probe + (
            "mode=no-dedup-unique",
            "second_listener_enter=0",
            "ambiguous_identity=0",
            "second_listener_identity=0",
        )
        mode = "no-dedup-unique"
    elif "mode=dedup-shared" in producer:
        producer_markers = (
            "instrumented_aliases=2",
            "add_int_aliases=2",
        )
        probe_markers = common_probe + (
            "mode=dedup-shared",
            "second_listener_enter=5",
            "ambiguous_identity=1",
            "second_listener_identity=1",
        )
        mode = "dedup-shared"
    else:
        return Check(
            "Code identity semantics",
            False,
            f"unknown producer mode: {producer.strip()}",
        )

    missing_producer = [marker for marker in producer_markers if marker not in producer]
    missing_probe = [marker for marker in probe_markers if marker not in probe]
    passed = not missing_producer and not missing_probe
    detail = f"mode={mode} producer/probe evidence verified"
    if not passed:
        detail = f"mode={mode} producer_missing={missing_producer} probe_missing={missing_probe}"
    return Check("Code identity semantics", passed, detail)


_THREADTIME_PID_RE = re.compile(
    r"^\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+\s+(\d+)\s+(\d+)\s+[VDIWEFAS]\s+"
)


def _crash_hits(log_text: str, metadata: dict[str, object]) -> list[str]:
    lines = log_text.splitlines()
    package = str(metadata.get("package", ""))
    pid_tokens = str(metadata.get("pid", "")).split()
    pid = pid_tokens[0] if pid_tokens else ""
    markers = (
        "Fatal signal",
        "FATAL EXCEPTION",
        "Abort message:",
        "SIGSEGV",
        "SIGABRT",
        "SIGBUS",
        "SIGILL",
        "Check failed:",
        "Assertion failed",
        "stack corruption",
        "double free",
        "use-after-free",
    )
    hits: list[str] = []
    pid_re = re.compile(rf"\b{re.escape(pid)}\b") if pid else None
    for line in lines:
        if not any(marker in line for marker in markers):
            continue

        # Logcat is a system-wide stream. Do not associate an unrelated fatal
        # record with the fixture merely because fixture output happened to be
        # interleaved a few lines away. Prefer the emitting PID from threadtime
        # format; for crash/tombstone records without that provenance, require
        # the fixture package or PID on the fatal line itself.
        source_match = _THREADTIME_PID_RE.match(line)
        source_pid = source_match.group(1) if source_match else ""
        if source_pid:
            if pid and source_pid == pid:
                hits.append(line.strip())
            elif package and package in line:
                hits.append(line.strip())
            continue

        if (package and package in line) or (pid_re is not None and pid_re.search(line)):
            hits.append(line.strip())
    return hits


def analyze(
    log_text: str,
    metadata: dict[str, object],
    *,
    expected_flutter: str,
    expected_dart: str,
    expected_test: str = "all",
    runtime_tier: str = "native",
) -> Analysis:
    events, malformed = parse_events(log_text)
    checks: list[Check] = [
        Check(
            "Structured event syntax",
            not malformed,
            f"events={len(events)} malformed={len(malformed)}",
        ),
        _check_device(metadata, runtime_tier),
        _check_runtime(events, expected_flutter, expected_dart),
        _check_core_binding(events),
    ]
    if runtime_tier == "translated-smoke":
        checks.extend(_check_scenarios(events, expected_test))
        crash_hits = _crash_hits(log_text, metadata)
        checks.append(
            Check(
                "Crash/anomaly scan",
                not crash_hits,
                "no process-scoped fatal markers" if not crash_hits else crash_hits[0],
            )
        )
        return Analysis(
            flutter=expected_flutter,
            dart=expected_dart,
            test=expected_test,
            runtime_tier=runtime_tier,
            checks=tuple(checks),
            malformed_events=tuple(malformed),
            crash_hits=tuple(crash_hits),
        )
    lifecycle_check, generation, isolate_generation = _select_lifecycle(events)
    checks.append(lifecycle_check)
    checks.extend(_check_capabilities(events, generation, isolate_generation))
    checks.extend(_check_scenarios(events, expected_test))
    if expected_test in {"all", "generic_gc"}:
        checks.append(_check_generic_gc(events))
    checks.extend(_check_legacy_proofs(log_text, expected_test))
    checks.append(_check_code_identity_semantics(log_text))
    crash_hits = _crash_hits(log_text, metadata)
    checks.append(
        Check(
            "Crash/anomaly scan",
            not crash_hits,
            "no process-scoped fatal markers" if not crash_hits else crash_hits[0],
        )
    )
    return Analysis(
        flutter=expected_flutter,
        dart=expected_dart,
        test=expected_test,
        runtime_tier=runtime_tier,
        checks=tuple(checks),
        malformed_events=tuple(malformed),
        crash_hits=tuple(crash_hits),
    )


def _write_json(path: Path, analysis: Analysis) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "flutter": analysis.flutter,
        "dart": analysis.dart,
        "test": analysis.test,
        "runtime_tier": analysis.runtime_tier,
        "result": "PASS" if analysis.passed else "FAIL",
        "checks": [asdict(check) for check in analysis.checks],
        "malformed_events": list(analysis.malformed_events),
        "crash_hits": list(analysis.crash_hits),
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def _markdown(analysis: Analysis) -> str:
    tier_label = (
        "native ARM64" if analysis.runtime_tier == "native" else "API 35 translated ARM smoke"
    )
    lines = [
        f"## DartPlant {tier_label} — Flutter {analysis.flutter} / Dart {analysis.dart}",
        "",
        "| Check | Result | Detail |",
        "| --- | --- | --- |",
    ]
    for check in analysis.checks:
        detail = check.detail.replace("|", "\\|").replace("\n", " ")
        lines.append(f"| {check.name} | {'PASS' if check.passed else 'FAIL'} | {detail} |")
    lines.extend(
        [
            "",
            f"**RESULT: {'PASS' if analysis.passed else 'FAIL'}**",
            "",
        ]
    )
    return "\n".join(lines)


def _write_junit(path: Path, analysis: Analysis) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    failures = sum(not check.passed for check in analysis.checks)
    suite = ET.Element(
        "testsuite",
        {
            "name": f"DartPlant Flutter {analysis.flutter} / Dart {analysis.dart}",
            "tests": str(len(analysis.checks)),
            "failures": str(failures),
        },
    )
    for check in analysis.checks:
        case = ET.SubElement(suite, "testcase", {"name": check.name})
        if not check.passed:
            failure = ET.SubElement(case, "failure", {"message": check.detail[:500]})
            failure.text = check.detail
    tree = ET.ElementTree(suite)
    ET.indent(tree, space="  ")
    tree.write(path, encoding="utf-8", xml_declaration=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Gate a DartPlant Flutter runtime from structured and raw logcat evidence"
    )
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--flutter", required=True)
    parser.add_argument("--dart", required=True)
    parser.add_argument("--test", default="all", choices=("all", *RUNTIME_SCENARIOS))
    parser.add_argument(
        "--runtime-tier", default="native", choices=("native", "translated-smoke")
    )
    parser.add_argument("--report-json", type=Path, required=True)
    parser.add_argument("--summary-md", type=Path, required=True)
    parser.add_argument("--junit", type=Path, required=True)
    args = parser.parse_args()

    log_text = args.log.read_text(errors="replace")
    metadata = json.loads(args.metadata.read_text())
    if not isinstance(metadata, dict):
        raise ValueError("runner metadata must be a JSON object")
    analysis = analyze(
        log_text,
        metadata,
        expected_flutter=args.flutter,
        expected_dart=args.dart,
        expected_test=args.test,
        runtime_tier=args.runtime_tier,
    )
    _write_json(args.report_json, analysis)
    summary = _markdown(analysis)
    args.summary_md.parent.mkdir(parents=True, exist_ok=True)
    args.summary_md.write_text(summary)
    sys.stdout.write(summary)
    return 0 if analysis.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
