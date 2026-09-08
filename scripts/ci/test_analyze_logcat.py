from __future__ import annotations

import copy
import json
import unittest

import analyze_logcat
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
            capabilities="0x3f7",
            required="0x3f7",
            verified_before="0x237",
            verified_invalidated="0x17",
            verified_revalidated="0x237",
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
    for name in analyze_logcat.REQUIRED_CAPABILITIES:
        lines.append(
            _event(
                event="capability",
                name=name,
                state="verified",
                capability="0x40",
                verified="0x3f7",
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
        for index, line in enumerate(lines):
            if '"event":"capability"' in line and '"name":"generated-transition"' in line:
                event = json.loads(line.split(analyze_logcat.CI_PREFIX, 1)[1])
                event["artifact_generation"] = 1
                lines[index] = "I/DartPlant: DARTPLANT_CI " + json.dumps(
                    event, separators=(",", ":")
                )
                break
        analysis = self._analyze(lines)
        self.assertFalse(analysis.passed)
        failed = {check.name for check in analysis.checks if not check.passed}
        self.assertIn("Capability generated-transition", failed)

    def test_stale_isolate_generation_fails(self) -> None:
        lines = _passing_lines()
        for index, line in enumerate(lines):
            if '"event":"capability"' in line and '"name":"active-exception"' in line:
                event = json.loads(line.split(analyze_logcat.CI_PREFIX, 1)[1])
                event["isolate_generation"] = 2
                lines[index] = "I/DartPlant: DARTPLANT_CI " + json.dumps(
                    event, separators=(",", ":")
                )
                break
        analysis = self._analyze(lines)
        self.assertFalse(analysis.passed)
        failed = {check.name for check in analysis.checks if not check.passed}
        self.assertIn("Capability active-exception", failed)

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
