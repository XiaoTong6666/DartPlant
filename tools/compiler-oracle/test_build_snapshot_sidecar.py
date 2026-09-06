#!/usr/bin/env python3

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from build_snapshot_sidecar import (
    AbiEvidence,
    ElfIdentity,
    LoadSegment,
    _derive_closure_abi_from_cfg,
    _extract_abi,
    _extract_entry_points,
    _extract_code_identity_profile,
    _extract_machine_code,
    _find_unique_executable_code,
    _load_abi_oracle,
    _normalize_aarch64_direct_branches,
    _expected_register_representations,
    _run_machine_code_analyzer,
    _validate_machine_code_cross_check,
    _validate_cfg_cross_check,
    _write_header,
)
from run_abi_oracle import _package_language_version


class ProfileBuilder:
    def __init__(self) -> None:
        self.strings = [""]
        self.string_ids = {"": 0}
        self.nodes: list[tuple[str, str, list[tuple[str, str, int]]]] = []

    def string_id(self, value: str) -> int:
        found = self.string_ids.get(value)
        if found is not None:
            return found
        index = len(self.strings)
        self.strings.append(value)
        self.string_ids[value] = index
        return index

    def add_node(self, node_type: str, name: str) -> int:
        index = len(self.nodes)
        self.nodes.append((node_type, name, []))
        return index

    def edge(self, source: int, name: str, target: int) -> None:
        self.nodes[source][2].append(("property", name, target))

    def write(self, path: Path) -> None:
        node_types = ["Function", "Class", "Library", "Code"]
        edge_types = ["context", "element", "property", "internal"]
        node_width = 5
        nodes: list[int] = []
        edges: list[int] = []
        for index, (node_type, name, outgoing) in enumerate(self.nodes):
            nodes.extend(
                [
                    node_types.index(node_type),
                    self.string_id(name),
                    index + 1,
                    0,
                    len(outgoing),
                ]
            )
            for edge_type, edge_name, target in outgoing:
                edges.extend(
                    [
                        edge_types.index(edge_type),
                        self.string_id(edge_name),
                        target * node_width,
                    ]
                )
        path.write_text(
            json.dumps(
                {
                    "snapshot": {
                        "meta": {
                            "node_fields": ["type", "name", "id", "self_size", "edge_count"],
                            "node_types": [node_types],
                            "edge_fields": ["type", "name_or_index", "to_node"],
                            "edge_types": [edge_types],
                        }
                    },
                    "nodes": nodes,
                    "edges": edges,
                    "strings": self.strings,
                }
            )
        )


class MachineCodeCrossCheckTests(unittest.TestCase):
    def test_compiler_entry_family_preserves_aliasing_and_function_kind(self) -> None:
        log = """Code for optimized function 'package:fixture/main.dart_::_target' (RegularFunction) {
0x1000    d503201f               nop
0x1004    d65f03c0               ret
}
Entry points for function 'package:fixture/main.dart_::_target' {
  [code+0x07] 1000 kNormal
  [code+0x0f] 1000 kMonomorphic
  [code+0x17] 1004 kUnchecked
  [code+0x1f] 1004 kMonomorphicUnchecked
}
"""
        machine = _extract_machine_code(log, "target")
        self.assertEqual("RegularFunction", machine.function_kind)
        self.assertEqual(0x1000, machine.start_address)
        self.assertEqual(bytes.fromhex("1f2003d5c0035fd6"), machine.code)
        self.assertEqual(
            {
                "kNormal": 0x1000,
                "kMonomorphic": 0x1000,
                "kUnchecked": 0x1004,
                "kMonomorphicUnchecked": 0x1004,
            },
            _extract_entry_points(log, machine.printed_name),
        )

    def test_compiler_entry_family_rejects_missing_entry_kind(self) -> None:
        log = """Entry points for function 'package:fixture/main.dart_::_target' {
  [code+0x07] 1000 kNormal
  [code+0x17] 1004 kUnchecked
}
"""
        with self.assertRaisesRegex(ValueError, "entry-point family.*incomplete"):
            _extract_entry_points(log, "package:fixture/main.dart_::_target")

    def test_cli_analyzes_real_machine_bytes(self) -> None:
        root = Path(__file__).resolve().parents[2]
        analyzer = root / "build" / "host" / "dartplant_aot_abi_analyzer_cli"
        if not analyzer.is_file():
            self.skipTest(f"host analyzer was not built: {analyzer}")
        document = _run_machine_code_analyzer(
            analyzer,
            bytes.fromhex("0028611ec0035fd6"),  # fadd d0, d0, d1; ret
            0x4000,
        )
        self.assertEqual(1, document["schema_version"])
        self.assertEqual(2, document["decoded_instructions"])
        self.assertEqual([0x4004], document["return_sites"])
        self.assertTrue(document["reached_return"])

    def test_cli_rejects_truncated_structural_fact_set(self) -> None:
        root = Path(__file__).resolve().parents[2]
        analyzer = root / "build" / "host" / "dartplant_aot_abi_analyzer_cli"
        if not analyzer.is_file():
            self.skipTest(f"host analyzer was not built: {analyzer}")
        code = bytes.fromhex("00000094") * 65 + bytes.fromhex("c0035fd6")
        with self.assertRaisesRegex(ValueError, "exceeded its fixed proof capacity"):
            _run_machine_code_analyzer(analyzer, code, 0x8000)

    def test_independent_gp_and_fpu_allocators_match_dart_arm64(self) -> None:
        abi = AbiEvidence(
            parameters=("unboxed-double", "unboxed-int64", "unboxed-double", "tagged"),
            result="tagged",
            max_parameters_in_registers=4,
            must_use_stack_calling_convention=False,
        )
        self.assertEqual(
            {
                ("fpu", 0): "unboxed-double",
                ("gp", 1): "unboxed-int64",
                ("fpu", 1): "unboxed-double",
                ("gp", 2): "tagged",
            },
            _expected_register_representations(abi),
        )

    def test_machine_code_known_fact_must_agree_with_compiler_truth(self) -> None:
        abi = AbiEvidence(
            parameters=("unboxed-double",),
            result="unboxed-double",
            max_parameters_in_registers=1,
            must_use_stack_calling_convention=False,
        )
        _validate_machine_code_cross_check(
            abi,
            {
                "observations": [
                    {
                        "location": "fpu",
                        "register": 0,
                        "stack_offset": 0,
                        "representation": "unboxed-double",
                    }
                ]
            },
        )
        with self.assertRaisesRegex(ValueError, "disagrees with machine-code"):
            _validate_machine_code_cross_check(
                abi,
                {
                    "observations": [
                        {
                            "location": "fpu",
                            "register": 0,
                            "stack_offset": 0,
                            "representation": "unboxed-int64",
                        }
                    ]
                },
            )

    def test_machine_code_unknown_and_stack_facts_do_not_override_metadata(self) -> None:
        abi = AbiEvidence(
            parameters=("unboxed-double",),
            result="unboxed-double",
            max_parameters_in_registers=0,
            must_use_stack_calling_convention=True,
        )
        _validate_machine_code_cross_check(
            abi,
            {
                "observations": [
                    {
                        "location": "entry-stack",
                        "register": 0,
                        "stack_offset": 16,
                        "representation": "unboxed-double",
                    },
                    {
                        "location": "fpu",
                        "register": 0,
                        "stack_offset": 0,
                        "representation": "unknown",
                    },
                ]
            },
        )


def make_profile(path: Path, *, shared: bool) -> None:
    builder = ProfileBuilder()
    library = builder.add_node("Library", "package:fixture/main.dart")
    owner = builder.add_node("Class", "::")
    target = builder.add_node("Function", "target")
    code = builder.add_node("Code", "[Optimized] target")
    builder.edge(owner, "library_", library)
    builder.edge(target, "owner_", owner)
    builder.edge(target, "code_", code)
    if shared:
        alias = builder.add_node("Function", "alias")
        builder.edge(alias, "owner_", owner)
        builder.edge(alias, "code_", code)
    builder.write(path)


class CodeIdentityProfileTest(unittest.TestCase):
    def test_unique_code_has_one_global_function_reference(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            profile = Path(temp) / "profile.json"
            make_profile(profile, shared=False)
            evidence = _extract_code_identity_profile(
                profile, "package:fixture/main.dart", "Global", "target"
            )
            self.assertEqual("unique", evidence.proof)
            self.assertEqual(1, evidence.physical_entry_alias_count)

    def test_shared_code_counts_alias_outside_target_record(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            profile = Path(temp) / "profile.json"
            make_profile(profile, shared=True)
            evidence = _extract_code_identity_profile(
                profile, "package:fixture/main.dart", "Global", "target"
            )
            self.assertEqual("shared", evidence.proof)
            self.assertEqual(2, evidence.physical_entry_alias_count)


class AbiOracleJsonTest(unittest.TestCase):
    def test_top_level_global_identity_uses_metadata_truth(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            oracle = Path(temp) / "oracle.json"
            oracle.write_text(
                json.dumps(
                    {
                        "format": 1,
                        "source": "vm.unboxing-info.metadata",
                        "functions": [
                            {
                                "library_uri": "package:fixture/main.dart",
                                "class_name": "",
                                "function_name": "target",
                                "parameters": ["tagged", "unboxed-double"],
                                "result": "unboxed-double",
                                "fixed_parameter_count": 2,
                                "has_optional_parameters": False,
                                "must_use_stack_calling_convention": False,
                                "has_overrides_with_less_direct_parameters": False,
                                "max_parameters_in_registers": 2,
                            }
                        ],
                    }
                )
            )
            evidence = _load_abi_oracle(
                oracle, "package:fixture/main.dart", "Global", "target"
            )
            self.assertEqual(("tagged", "unboxed-double"), evidence.parameters)
            self.assertEqual("unboxed-double", evidence.result)
            self.assertEqual(2, evidence.max_parameters_in_registers)

    def test_unknown_representation_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            oracle = Path(temp) / "oracle.json"
            oracle.write_text(
                json.dumps(
                    {
                        "format": 1,
                        "source": "vm.unboxing-info.metadata",
                        "functions": [
                            {
                                "library_uri": "package:fixture/main.dart",
                                "class_name": "Fixture",
                                "function_name": "target",
                                "parameters": ["unknown"],
                                "result": "tagged",
                                "fixed_parameter_count": 1,
                                "has_optional_parameters": False,
                                "must_use_stack_calling_convention": False,
                                "has_overrides_with_less_direct_parameters": False,
                                "max_parameters_in_registers": 1,
                            }
                        ],
                    }
                )
            )
            with self.assertRaises(ValueError):
                _load_abi_oracle(oracle, "package:fixture/main.dart", "Fixture", "target")


class OracleRunnerTest(unittest.TestCase):
    def test_sdk_constraint_derives_package_language_floor(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            pubspec = Path(temp) / "pubspec.yaml"
            pubspec.write_text("name: vm\nenvironment:\n  sdk: '^3.12.0-0'\n")
            self.assertEqual("3.12", _package_language_version(pubspec))
            pubspec.write_text("name: vm\nenvironment:\n  sdk: '>=3.3.0 <4.0.0'\n")
            self.assertEqual("3.3", _package_language_version(pubspec))


class GeneratedBundleTest(unittest.TestCase):
    def test_generated_header_self_registers_artifact_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            header = Path(temp) / "sidecar.h"
            _write_header(
                header,
                {
                    "library_uri": "package:fixture/main.dart",
                    "class_name": "Global",
                    "function_name": "target",
                    "entry_va": 0x1234,
                    "code_size": 16,
                    "fingerprint": "0123456789abcdef",
                    "build_id": "00112233",
                    "snapshot_hash": "0123456789abcdef0123456789abcdef",
                    "code_identity_proof": "unique",
                    "physical_entry_alias_count": 1,
                    "abi_parameters": ["unboxed-double"],
                    "abi_result": "unboxed-double",
                    "max_parameters_in_registers": 1,
                    "must_use_stack_calling_convention": False,
                    "has_optional_parameters": False,
                    "has_overrides_with_less_direct_parameters": False,
                },
            )
            text = header.read_text()
            self.assertIn('#include "dartplant/advanced/artifact.h"', text)
            self.assertIn("DartPlantArtifactBundle", text)
            self.assertIn("dartplant_register_embedded_artifact_bundle", text)
            self.assertIn(".structural_schema_version = 0", text)

    def test_generated_header_carries_verified_structural_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            header = Path(temp) / "sidecar.h"
            _write_header(
                header,
                {
                    "library_uri": "package:fixture/main.dart",
                    "class_name": "Global",
                    "function_name": "target",
                    "entry_va": 0x1234,
                    "code_size": 16,
                    "fingerprint": "0123456789abcdef",
                    "build_id": "00112233",
                    "snapshot_hash": "0123456789abcdef0123456789abcdef",
                    "code_identity_proof": "unique",
                    "physical_entry_alias_count": 1,
                    "abi_parameters": ["unboxed-double"],
                    "abi_result": "unboxed-double",
                    "max_parameters_in_registers": 1,
                    "must_use_stack_calling_convention": False,
                    "has_optional_parameters": False,
                    "has_overrides_with_less_direct_parameters": False,
                    "structural_analysis": {
                        "decoded_instructions": 4,
                        "basic_block_count": 1,
                        "has_unknown_control_flow": False,
                        "uses_arguments_descriptor": True,
                        "reached_return": True,
                        "direct_call_count": 1,
                        "return_site_count": 1,
                        "external_branch_count": 0,
                        "indirect_call_count": 0,
                        "indirect_branch_count": 0,
                        "address_materialization_count": 2,
                    },
                },
            )
            text = header.read_text()
            self.assertIn(".structural_schema_version = 1", text)
            self.assertIn(".structural_verified = 1", text)
            self.assertIn(".structural_relation_count = 5", text)
            self.assertIn(".structural_uses_arguments_descriptor = 1", text)
            self.assertIn(".structural_reached_return = 1", text)

    def test_generated_header_supports_unique_symbol_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            header = Path(temp) / "sidecar.h"
            _write_header(
                header,
                {
                    "library_uri": "package:fixture/main.dart",
                    "class_name": "Global",
                    "function_name": "target",
                    "entry_va": 0x1234,
                    "code_size": 16,
                    "fingerprint": "0123456789abcdef",
                    "build_id": "00112233",
                    "snapshot_hash": "0123456789abcdef0123456789abcdef",
                    "code_identity_proof": "unique",
                    "physical_entry_alias_count": 1,
                    "abi_parameters": ["unboxed-int64"],
                    "abi_result": "unboxed-int64",
                    "max_parameters_in_registers": 1,
                    "must_use_stack_calling_convention": False,
                    "has_optional_parameters": False,
                    "has_overrides_with_less_direct_parameters": False,
                },
                "DartPlantP6Int64",
            )
            text = header.read_text()
            self.assertIn("kDartPlantP6Int64ArtifactBundle", text)
            self.assertIn("DartPlantP6Int64ArtifactRegistrar", text)
            self.assertNotIn("kDartPlantOrdinaryAotArtifactBundle", text)

    def test_generated_header_binds_exact_non_default_entry_and_closure_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            header = Path(temp) / "sidecar.h"
            _write_header(
                header,
                {
                    "library_uri": "package:fixture/main.dart",
                    "class_name": "Global",
                    "function_name": "target",
                    "entry_kind": "unchecked",
                    "entry_va": 0x1240,
                    "code_size": 12,
                    "code_payload_va": 0x1234,
                    "code_instructions_length": 24,
                    "fingerprint": "0123456789abcdef",
                    "build_id": "00112233",
                    "snapshot_hash": "0123456789abcdef0123456789abcdef",
                    "code_identity_proof": "unique",
                    "physical_entry_alias_count": 1,
                    "function_kind": 0,
                    "closure_call_entry_only": False,
                    "abi_parameters": ["tagged"],
                    "abi_result": "tagged",
                    "max_parameters_in_registers": 1,
                    "must_use_stack_calling_convention": False,
                    "has_optional_parameters": False,
                    "has_overrides_with_less_direct_parameters": False,
                },
            )
            text = header.read_text()
            self.assertGreaterEqual(text.count(".entry_kind = DARTPLANT_ENTRY_UNCHECKED"), 2)
            self.assertIn(".function_kind = 0u", text)
            self.assertIn(".closure_call_entry_only = 0", text)
            self.assertIn(".code_payload_va = 0x1234ULL", text)
            self.assertIn(".code_instructions_length = 24ULL", text)


class ClosureAbiDerivationTest(unittest.TestCase):
    def test_exact_synthetic_cfg_strips_hidden_closure_and_keeps_user_stack(self) -> None:
        log = """*** BEGIN CFG
After AllocateRegisters
==== package:fixture/main.dart_::_target_target (ImplicitClosureFunction)
  2: B2[function entry]:2 {
      v2 <- Parameter(0 @fp[4]) T{*?}
      v3 <- Parameter(1 @fp[3]) T{int}
      v4 <- Parameter(2 @fp[2]) T{int}
}
  8:     DartReturn:8(v3)
*** END CFG
"""
        cfg = _extract_abi(
            log,
            "target",
            exact_printed_name="package:fixture/main.dart_::_target_target",
        )
        parent = AbiEvidence(
            parameters=("tagged", "tagged"),
            result="tagged",
            max_parameters_in_registers=0,
            must_use_stack_calling_convention=True,
        )
        closure = _derive_closure_abi_from_cfg(parent, cfg)
        self.assertEqual(("tagged", "tagged"), closure.parameters)
        self.assertEqual("tagged", closure.result)
        self.assertTrue(closure.must_use_stack_calling_convention)
        self.assertEqual(0, closure.max_parameters_in_registers)

    def test_synthetic_cfg_rejects_unboxed_or_wrong_formal_count(self) -> None:
        parent = AbiEvidence(
            parameters=("tagged", "tagged"),
            result="tagged",
            max_parameters_in_registers=0,
            must_use_stack_calling_convention=True,
        )
        with self.assertRaisesRegex(ValueError, "boxed synthetic-closure parameter"):
            _derive_closure_abi_from_cfg(
                parent,
                AbiEvidence(
                    parameters=("tagged", "unboxed-int64", "tagged"),
                    result="tagged",
                    max_parameters_in_registers=0,
                    must_use_stack_calling_convention=True,
                ),
            )

    def test_optional_generic_closure_uses_arguments_descriptor_stack_dispatch(self) -> None:
        log = """*** BEGIN CFG
After AllocateRegisters
==== package:fixture/main.dart_::_target_target (ImplicitClosureFunction)
  2: B1[function entry]:2 {
      v2 <- Parameter(5 @r4) T{*?}
}
  4:     v4 <- LoadField(v2 . ArgumentsDescriptor.count {final}) T{_Smi}
  6:     v7 <- LoadIndexedUnsafe(fp[v4 + 16]) T{Y0?}
  8:     v8 <- LoadField(v2 . ArgumentsDescriptor.type_args_len {final}) T{_Smi}
 10:     v9 <- StaticCall:8( target<1> v7) T{Y0?}
 12:     DartReturn:10(v9)
*** END CFG
"""
        with self.assertRaisesRegex(ValueError, "non-contiguous formal parameter indexes"):
            _extract_abi(
                log,
                "target",
                exact_printed_name="package:fixture/main.dart_::_target_target",
            )
        cfg = _extract_abi(
            log,
            "target",
            exact_printed_name="package:fixture/main.dart_::_target_target",
            allow_sparse_parameter_indexes=True,
        )
        parent = AbiEvidence(
            parameters=("tagged",),
            result="tagged",
            max_parameters_in_registers=0,
            must_use_stack_calling_convention=True,
            has_optional_parameters=True,
        )
        closure = _derive_closure_abi_from_cfg(parent, cfg)
        self.assertEqual(("tagged",), closure.parameters)
        self.assertEqual("tagged", closure.result)
        self.assertTrue(closure.must_use_stack_calling_convention)
        self.assertEqual(0, closure.max_parameters_in_registers)
        self.assertTrue(closure.has_optional_parameters)

    def test_optional_closure_rejects_non_r4_dispatch_parameter(self) -> None:
        parent = AbiEvidence(
            parameters=("tagged",),
            result="tagged",
            max_parameters_in_registers=0,
            must_use_stack_calling_convention=True,
            has_optional_parameters=True,
        )
        with self.assertRaisesRegex(ValueError, "ArgumentsDescriptor in r4"):
            _derive_closure_abi_from_cfg(
                parent,
                AbiEvidence(
                    parameters=("tagged",),
                    result="tagged",
                    max_parameters_in_registers=1,
                    must_use_stack_calling_convention=False,
                    parameter_locations=("r5",),
                    uses_arguments_descriptor=True,
                    loads_entry_stack=True,
                ),
            )
        non_optional_parent = AbiEvidence(
            parameters=("tagged", "tagged"),
            result="tagged",
            max_parameters_in_registers=0,
            must_use_stack_calling_convention=True,
        )
        with self.assertRaisesRegex(ValueError, "formal count"):
            _derive_closure_abi_from_cfg(
                non_optional_parent,
                AbiEvidence(
                    parameters=("tagged", "tagged"),
                    result="tagged",
                    max_parameters_in_registers=0,
                    must_use_stack_calling_convention=True,
                ),
            )


class CfgCrossCheckTest(unittest.TestCase):
    def test_unknown_cfg_result_does_not_override_metadata_truth(self) -> None:
        oracle = AbiEvidence(
            parameters=("tagged", "tagged"),
            result="tagged",
            max_parameters_in_registers=0,
            must_use_stack_calling_convention=True,
        )
        cfg = AbiEvidence(
            parameters=("tagged", "tagged"),
            result="unknown",
            max_parameters_in_registers=0,
            must_use_stack_calling_convention=True,
        )
        _validate_cfg_cross_check(oracle, cfg)

    def test_known_cfg_conflict_still_fails_closed(self) -> None:
        oracle = AbiEvidence(
            parameters=("unboxed-int64",),
            result="unboxed-int64",
            max_parameters_in_registers=1,
            must_use_stack_calling_convention=False,
        )
        cfg = AbiEvidence(
            parameters=("unboxed-double",),
            result="unboxed-int64",
            max_parameters_in_registers=1,
            must_use_stack_calling_convention=False,
        )
        with self.assertRaises(ValueError):
            _validate_cfg_cross_check(oracle, cfg)


class ExecutableCodeLocatorTest(unittest.TestCase):
    def test_exact_match_preserves_target_bytes(self) -> None:
        code = bytes.fromhex("fd7bbfa9fd030091c0035fd6")
        data = b"\0" * 16 + code + b"\0" * 16
        identity = ElfIdentity(
            build_id="test",
            snapshot_hash="test",
            load_segments=(LoadSegment(0, 0x1000, len(data), 0x1),),
        )
        va, actual, mode = _find_unique_executable_code(data, identity, code)
        self.assertEqual(0x1010, va)
        self.assertEqual(code, actual)
        self.assertEqual("exact", mode)

    def test_branch_immediate_fallback_uses_actual_target_bytes(self) -> None:
        # Same Function shape with only a BL imm26 displacement changed by the
        # diagnostic gen_snapshot layout. Offline matching may mask that field,
        # but the returned fingerprint source must be the exact target bytes.
        oracle = bytes.fromhex("fd7bbfa900010094c0035fd6")
        actual = bytes.fromhex("fd7bbfa923010094c0035fd6")
        data = b"\0" * 32 + actual + b"\0" * 32
        identity = ElfIdentity(
            build_id="test",
            snapshot_hash="test",
            load_segments=(LoadSegment(0, 0x2000, len(data), 0x1),),
        )
        va, matched, mode = _find_unique_executable_code(data, identity, oracle)
        self.assertEqual(0x2020, va)
        self.assertEqual(actual, matched)
        self.assertEqual("branch-relocation-normalized", mode)

    def test_branch_immediate_fallback_rejects_ambiguous_matches(self) -> None:
        oracle = bytes.fromhex("fd7bbfa900010094c0035fd6")
        first = bytes.fromhex("fd7bbfa923010094c0035fd6")
        second = bytes.fromhex("fd7bbfa945020094c0035fd6")
        data = b"\0" * 16 + first + b"\0" * 16 + second + b"\0" * 16
        identity = ElfIdentity(
            build_id="test",
            snapshot_hash="test",
            load_segments=(LoadSegment(0, 0x3000, len(data), 0x1),),
        )
        with self.assertRaises(ValueError):
            _find_unique_executable_code(data, identity, oracle)

    def test_branch_normalization_preserves_intra_function_targets(self) -> None:
        # At offset 4, B +4 targets offset 8 and B -4 targets offset 0. Both
        # remain inside the Function and therefore must stay identity-bearing.
        first = bytes.fromhex("fd7bbfa901000014c0035fd6")
        second = bytes.fromhex("fd7bbfa9ffffff17c0035fd6")
        self.assertEqual(first, _normalize_aarch64_direct_branches(first))
        self.assertEqual(second, _normalize_aarch64_direct_branches(second))
        self.assertNotEqual(
            _normalize_aarch64_direct_branches(first),
            _normalize_aarch64_direct_branches(second),
        )


if __name__ == "__main__":
    unittest.main()
