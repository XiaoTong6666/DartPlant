from __future__ import annotations

import copy
import json
import unittest

import analyze_logcat
from capability_registry import mask as capability_mask
from capability_registry import required_event_capabilities
from common import RUNTIME_SCENARIOS


def _event(**fields: object) -> str:
    return "I/DartPlant: DARTPLANT_CI " + json.dumps(fields, separators=(",", ":"))


def _metadata() -> dict[str, object]:
    return {
        "package": "dev.dartplant.dartplant_fixture",
        "pid": "4242",
        "runner_state": "suite_seen",
        "guest_abi": "arm64-v8a",
        "guest_uname_m": "aarch64",
        "apk": {"abi": "arm64-v8a", "elf_machine": "AArch64"},
    }


def _passing_lines() -> list[str]:
    required_mask = capability_mask(cold_required=True)
    available_mask = required_mask | 0x10000
    verified_after_create = capability_mask(verified_after_create=True)
    lines = [
        _event(
            event="runtime",
            flutter="3.22.3",
            dart="3.4.4",
            dart_runtime="3.4.4",
            abi="arm64-v8a",
            dart_ffi_abi="android_arm64",
        ),
        _event(
            event="core_binding",
            state="verified",
            candidate_count=3,
            passing_count=1,
            distinct_abis=1,
        ),
        _event(
            event="artifact_lifecycle",
            state="pass",
            capabilities=hex(available_mask),
            required=hex(required_mask),
            verified_before=hex(verified_after_create),
            verified_invalidated="0x0",
            verified_revalidated=hex(verified_after_create),
            failed_before="0x0",
            failed_revalidated="0x0",
            generation_before=1,
            generation_quiesced=1,
            generation_invalidated=2,
            generation_revalidated=2,
            isolate_generation=1,
            quiesce_status=0,
            premature_revalidation_status=-17,
            retire_status=0,
            revalidation_status=0,
        ),
    ]
    for capability in required_event_capabilities():
        lines.append(
            _event(
                event="capability",
                schema_version=2,
                capability=capability.diagnostic_name,
                capability_bit=hex(capability.bit),
                state="verified",
                source_rows=3,
                root_compatible=2,
                relational_passed=2,
                distinct_keys=1,
                selected_rows=["A", "B"],
                verified=hex(available_mask),
                failed="0x0",
                artifact_generation=2,
                isolate_generation=1,
            )
        )
    for name in RUNTIME_SCENARIOS:
        lines.append(_event(event="scenario", name=name, state="pass"))
    lines.append(
        _event(
            event="type_arguments",
            state="pass",
            require_relocation=True,
            dart_api_calls=16384,
            parameter_before="0x7300000011",
            parameter_after="0x7400000011",
            parameter_relocated=True,
            vector_before="0x7300000101",
            vector_after="0x7300000101",
            element_before="0x7300000201",
            element_after="0x7300000201",
        )
    )
    lines.extend(
        [
            _event(event="suite", state="pass", test="all"),
        ]
    )
    for markers in analyze_logcat.LEGACY_PROOFS.values():
        lines.extend(markers)
    lines.extend(
        [
            "producer code-identity policy verified mode=dedup-shared "
            "instrumented_aliases=2 add_int_aliases=2",
            "instrumentedAdd probe mode=dedup-shared enter=5 leave=5 "
            "second_listener_enter=5 live_ok=5 live_failed=0 lookup_ok=1 model_ok=1 "
            "policy_ok=1 ambiguous_identity=1 second_listener_identity=1 "
            "result=115 expected=115",
        ]
    )
    return lines


class AnalyzeLogcatTest(unittest.TestCase):
    def _analyze(
        self, lines: list[str], metadata: dict[str, object] | None = None
    ) -> analyze_logcat.Analysis:
        return analyze_logcat.analyze(
            "\n".join(lines) + "\n",
            metadata or _metadata(),
            expected_flutter="3.22.3",
            expected_dart="3.4.4",
        )

    def test_passing_log(self) -> None:
        analysis = self._analyze(_passing_lines())
        self.assertTrue(analysis.passed, [check for check in analysis.checks if not check.passed])

    def test_stale_capability_generation_fails(self) -> None:
        lines = _passing_lines()
        capability_name = required_event_capabilities()[0].diagnostic_name
        for index, line in enumerate(lines):
            if '"event":"capability"' in line and f'"capability":"{capability_name}"' in line:
                event = json.loads(line.split(analyze_logcat.CI_PREFIX, 1)[1])
                event["artifact_generation"] = 1
                lines[index] = "I/DartPlant: DARTPLANT_CI " + json.dumps(
                    event, separators=(",", ":")
                )
                break
        analysis = self._analyze(lines)
        self.assertFalse(analysis.passed)
        failed = {check.name for check in analysis.checks if not check.passed}
        self.assertIn(f"Capability {capability_name}", failed)

    def test_stale_isolate_generation_fails(self) -> None:
        lines = _passing_lines()
        capability_name = required_event_capabilities()[1].diagnostic_name
        for index, line in enumerate(lines):
            if '"event":"capability"' in line and f'"capability":"{capability_name}"' in line:
                event = json.loads(line.split(analyze_logcat.CI_PREFIX, 1)[1])
                event["isolate_generation"] = 2
                lines[index] = "I/DartPlant: DARTPLANT_CI " + json.dumps(
                    event, separators=(",", ":")
                )
                break
        analysis = self._analyze(lines)
        self.assertFalse(analysis.passed)
        failed = {check.name for check in analysis.checks if not check.passed}
        self.assertIn(f"Capability {capability_name}", failed)

    def test_capability_superset_is_accepted(self) -> None:
        analysis = self._analyze(_passing_lines())
        lifecycle = next(check for check in analysis.checks if check.name == "Artifact lifecycle")
        self.assertTrue(lifecycle.passed, lifecycle.detail)

    def test_capability_ambiguity_is_distinguished(self) -> None:
        lines = _passing_lines()
        capability_name = required_event_capabilities()[0].diagnostic_name
        for index, line in enumerate(lines):
            if '"event":"capability"' in line and f'"capability":"{capability_name}"' in line:
                event = json.loads(line.split(analyze_logcat.CI_PREFIX, 1)[1])
                event.update(
                    state="ambiguous",
                    distinct_keys=2,
                    selected_rows=[],
                    failed=hex(required_event_capabilities()[0].bit),
                )
                lines[index] = "I/DartPlant: DARTPLANT_CI " + json.dumps(
                    event, separators=(",", ":")
                )
                break
        analysis = self._analyze(lines)
        failures = [check for check in analysis.checks if not check.passed]
        self.assertTrue(any("ambiguous" in check.detail for check in failures), failures)

    def test_capability_failure_states_remain_distinguishable(self) -> None:
        capability = required_event_capabilities()[0]
        for state in ("predicate_failed", "dependency_mismatch", "generation_stale"):
            with self.subTest(state=state):
                lines = _passing_lines()
                for index, line in enumerate(lines):
                    if (
                        '"event":"capability"' in line
                        and f'"capability":"{capability.diagnostic_name}"' in line
                    ):
                        event = json.loads(line.split(analyze_logcat.CI_PREFIX, 1)[1])
                        event["state"] = state
                        event["failed"] = hex(capability.bit)
                        if state == "predicate_failed":
                            event["relational_passed"] = 0
                            event["distinct_keys"] = 0
                            event["selected_rows"] = []
                        lines[index] = "I/DartPlant: DARTPLANT_CI " + json.dumps(
                            event, separators=(",", ":")
                        )
                        break
                analysis = self._analyze(lines)
                failure = next(
                    check for check in analysis.checks if check.name == "Capability failures"
                )
                self.assertFalse(failure.passed)
                self.assertIn(f"state={state}", failure.detail)

    def test_unknown_future_capability_event_is_ignored(self) -> None:
        lines = _passing_lines()
        lines.append(
            _event(
                event="capability",
                schema_version=2,
                capability="FutureCapability",
                capability_bit="0x20000",
                state="verified",
                source_rows=3,
                root_compatible=1,
                relational_passed=1,
                distinct_keys=1,
                selected_rows=["A"],
                verified="0x3ffff",
                failed="0x0",
                artifact_generation=2,
                isolate_generation=1,
            )
        )
        analysis = self._analyze(lines)
        self.assertTrue(analysis.passed, [check for check in analysis.checks if not check.passed])

    def test_process_scoped_crash_fails(self) -> None:
        lines = _passing_lines()
        lines.extend(["filler"] * 12)
        lines.append(
            "F/libc: Fatal signal 11 (SIGSEGV), code 1, pid 4242 "
            "(dev.dartplant.dartplant_fixture)"
        )
        analysis = self._analyze(lines)
        self.assertFalse(analysis.passed)
        self.assertTrue(analysis.crash_hits)

    def test_unrelated_crash_is_ignored(self) -> None:
        lines = _passing_lines()
        lines.extend(["filler"] * 12)
        lines.append("F/libc: Fatal signal 11 (SIGSEGV), code 1, pid 999 (com.example.other)")
        analysis = self._analyze(lines)
        self.assertTrue(analysis.passed, analysis.crash_hits)

    def test_interleaved_system_crash_is_not_attributed_to_fixture(self) -> None:
        lines = _passing_lines()
        lines.extend(
            [
                "09-09 08:28:20.850  4242  4242 I DartPlantFixture: fixture still alive",
                "09-09 08:28:20.862  2219  2250 F libc    : Fatal signal 6 (SIGABRT), "
                "code -1 (SI_QUEUE) in tid 2250 (alarm_default_c), pid 2219 (droid.bluetooth)",
                "09-09 08:28:20.870  4242  4242 I flutter : fixture continues",
            ]
        )
        analysis = self._analyze(lines)
        self.assertTrue(analysis.passed, analysis.crash_hits)
        self.assertEqual(analysis.crash_hits, ())

    def test_process_scoped_threadtime_crash_fails(self) -> None:
        lines = _passing_lines()
        lines.append(
            "09-09 08:28:20.862  4242  4250 F libc    : Fatal signal 11 (SIGSEGV), "
            "code 1, pid 4242 (dartplant_fixture)"
        )
        analysis = self._analyze(lines)
        self.assertFalse(analysis.passed)
        self.assertTrue(analysis.crash_hits)

    def test_profile_no_dedup_code_identity_semantics_pass(self) -> None:
        lines = _passing_lines()
        lines = [
            line
            for line in lines
            if "producer code-identity policy verified mode=" not in line
            and "instrumentedAdd probe mode=" not in line
        ]
        lines.extend(
            [
                "producer code-identity policy verified mode=no-dedup-unique "
                "instrumented_aliases=1 add_int_aliases=1",
                "instrumentedAdd probe mode=no-dedup-unique enter=5 leave=5 "
                "second_listener_enter=0 live_ok=5 live_failed=0 lookup_ok=1 model_ok=1 "
                "policy_ok=1 ambiguous_identity=0 second_listener_identity=0 "
                "result=115 expected=115",
            ]
        )
        analysis = self._analyze(lines)
        self.assertTrue(analysis.passed, [check for check in analysis.checks if not check.passed])

    def test_profile_no_dedup_wrong_alias_semantics_fail(self) -> None:
        lines = _passing_lines()
        lines = [
            line
            for line in lines
            if "producer code-identity policy verified mode=" not in line
            and "instrumentedAdd probe mode=" not in line
        ]
        lines.extend(
            [
                "producer code-identity policy verified mode=no-dedup-unique "
                "instrumented_aliases=2 add_int_aliases=2",
                "instrumentedAdd probe mode=no-dedup-unique enter=5 leave=5 "
                "second_listener_enter=0 live_ok=5 live_failed=0 lookup_ok=1 model_ok=1 "
                "policy_ok=1 ambiguous_identity=0 second_listener_identity=0 "
                "result=115 expected=115",
            ]
        )
        analysis = self._analyze(lines)
        self.assertFalse(analysis.passed)
        failure = next(
            check for check in analysis.checks if check.name == "Code identity semantics"
        )
        self.assertIn("instrumented_aliases=1", failure.detail)

    def test_prelaunch_runner_error_without_pid_fails_without_crashing(self) -> None:
        metadata = copy.deepcopy(_metadata())
        metadata.pop("pid", None)
        metadata["runner_state"] = "error"
        metadata["runner_error"] = "adb install failed before launch"
        analysis = self._analyze(
            ["F/libc: Fatal signal 11 (SIGSEGV), code 1, pid 999 (com.example.other)"],
            metadata,
        )
        self.assertFalse(analysis.passed)
        self.assertEqual(analysis.crash_hits, ())
        failed = {check.name for check in analysis.checks if not check.passed}
        self.assertIn("Native ARM64 provenance", failed)

    def test_runner_provenance_is_a_hard_gate(self) -> None:
        metadata = copy.deepcopy(_metadata())
        metadata["guest_uname_m"] = "x86_64"
        analysis = self._analyze(_passing_lines(), metadata)
        self.assertFalse(analysis.passed)
        failed = {check.name for check in analysis.checks if not check.passed}
        self.assertIn("Native ARM64 provenance", failed)

    def test_translated_smoke_accepts_x86_guest(self) -> None:
        metadata = copy.deepcopy(_metadata())
        metadata["guest_abi"] = "x86_64"
        metadata["guest_uname_m"] = "x86_64"
        lines = _passing_lines()
        analysis = analyze_logcat.analyze(
            "\n".join(lines) + "\n",
            metadata,
            expected_flutter="3.22.3",
            expected_dart="3.4.4",
            expected_test="normal",
            runtime_tier="translated-smoke",
        )
        self.assertFalse(analysis.passed)
        lines = [line for line in lines if '"event":"suite"' not in line]
        lines.append(_event(event="suite", state="pass", test="normal"))
        analysis = analyze_logcat.analyze(
            "\n".join(lines) + "\n",
            metadata,
            expected_flutter="3.22.3",
            expected_dart="3.4.4",
            expected_test="normal",
            runtime_tier="translated-smoke",
        )
        self.assertTrue(analysis.passed, [check for check in analysis.checks if not check.passed])

    def test_non_gc_selector_does_not_require_gc_legacy_markers(self) -> None:
        lines = _passing_lines()
        gc_markers = set(analyze_logcat.LEGACY_PROOFS["TypeArguments moving GC"])
        lines = [line for line in lines if line not in gc_markers]
        lines = [line for line in lines if '"event":"suite"' not in line]
        lines.append(_event(event="suite", state="pass", test="arguments_descriptor"))
        analysis = analyze_logcat.analyze(
            "\n".join(lines) + "\n",
            _metadata(),
            expected_flutter="3.22.3",
            expected_dart="3.4.4",
            expected_test="arguments_descriptor",
            runtime_tier="native",
        )
        self.assertTrue(analysis.passed, [check for check in analysis.checks if not check.passed])


if __name__ == "__main__":
    unittest.main()
