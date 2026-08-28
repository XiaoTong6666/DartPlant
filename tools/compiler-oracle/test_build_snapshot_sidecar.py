#!/usr/bin/env python3

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from build_snapshot_sidecar import _extract_code_identity_profile, _load_abi_oracle
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


if __name__ == "__main__":
    unittest.main()
