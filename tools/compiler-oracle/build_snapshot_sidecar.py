#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess as sp
import tempfile
from dataclasses import dataclass
from pathlib import Path


_CODE_HEADER_RE = re.compile(
    r"^Code for optimized function '(?P<name>.+)' \(RegularFunction\) \{$"
)
_INSTRUCTION_RE = re.compile(r"^0x[0-9a-fA-F]+\s+(?P<word>[0-9a-fA-F]{8})\s+")
_CFG_HEADER_RE = re.compile(r"^==== (?P<name>.+) \(RegularFunction\)$")
_PARAMETER_RE = re.compile(
    r"^\s*v(?P<value>\d+) <- Parameter\((?P<index>\d+) @(?P<location>[^)]+)\)(?P<tail>.*)$"
)
_VALUE_DEF_RE = re.compile(r"^\s*(?:\d+:\s+)?v(?P<value>\d+) <- (?P<body>.+)$")
_RETURN_RE = re.compile(r"^\s*\d+:\s+DartReturn(?::\d+)?\(v(?P<value>\d+)\)")
_SNAPSHOT_MAGIC = b"\xf5\xf5\xdc\xdc"
_HEX_32_RE = re.compile(rb"^[0-9a-fA-F]{32}$")


@dataclass(frozen=True)
class LoadSegment:
    file_offset: int
    virtual_address: int
    file_size: int
    flags: int


@dataclass(frozen=True)
class ElfIdentity:
    build_id: str
    snapshot_hash: str
    load_segments: tuple[LoadSegment, ...]


@dataclass(frozen=True)
class AbiEvidence:
    parameters: tuple[str, ...]
    result: str
    max_parameters_in_registers: int
    must_use_stack_calling_convention: bool
    has_optional_parameters: bool = False
    has_overrides_with_less_direct_parameters: bool = False


@dataclass(frozen=True)
class CodeIdentityEvidence:
    proof: str
    physical_entry_alias_count: int


def _load_abi_oracle(
    path: Path,
    library_uri: str,
    class_name: str,
    function_name: str,
) -> AbiEvidence:
    document = json.loads(path.read_text())
    if document.get("format") != 1 or document.get("source") != "vm.unboxing-info.metadata":
        raise ValueError("ABI oracle JSON has an unsupported format/source")
    functions = document.get("functions")
    if not isinstance(functions, list):
        raise ValueError("ABI oracle JSON has no functions array")

    expected_class = "" if class_name == "Global" else class_name
    matches = [
        function
        for function in functions
        if isinstance(function, dict)
        and function.get("library_uri") == library_uri
        and function.get("class_name") == expected_class
        and function.get("function_name") == function_name
    ]
    if len(matches) != 1:
        raise ValueError(
            "ABI oracle did not identify exactly one compiler Function for "
            f"{library_uri}/{class_name}/{function_name}: {len(matches)} candidates"
        )
    function = matches[0]
    parameters = function.get("parameters")
    result = function.get("result")
    if not isinstance(parameters, list) or not all(isinstance(value, str) for value in parameters):
        raise ValueError("ABI oracle parameter representation list is invalid")
    allowed_parameters = {"tagged", "unboxed-int64", "unboxed-double"}
    if any(value not in allowed_parameters for value in parameters):
        raise ValueError(f"ABI oracle contains an unsupported parameter representation: {parameters}")
    allowed_results = allowed_parameters | {"pair-of-tagged"}
    if not isinstance(result, str) or result not in allowed_results:
        raise ValueError(f"ABI oracle contains an unsupported result representation: {result!r}")

    fixed_parameter_count = function.get("fixed_parameter_count")
    max_parameters_in_registers = function.get("max_parameters_in_registers")
    if fixed_parameter_count != len(parameters):
        raise ValueError(
            "ABI oracle fixed-parameter count does not match its representation vector"
        )
    if (
        not isinstance(max_parameters_in_registers, int)
        or max_parameters_in_registers < 0
        or max_parameters_in_registers > len(parameters)
    ):
        raise ValueError("ABI oracle max_parameters_in_registers is invalid")
    must_use_stack = function.get("must_use_stack_calling_convention")
    has_optional = function.get("has_optional_parameters")
    has_overrides = function.get("has_overrides_with_less_direct_parameters")
    if not all(isinstance(value, bool) for value in (must_use_stack, has_optional, has_overrides)):
        raise ValueError("ABI oracle calling-convention flags are invalid")
    if must_use_stack and max_parameters_in_registers != 0:
        raise ValueError("ABI oracle forced-stack Function still reports register parameters")

    return AbiEvidence(
        parameters=tuple(parameters),
        result=result,
        max_parameters_in_registers=max_parameters_in_registers,
        must_use_stack_calling_convention=must_use_stack,
        has_optional_parameters=has_optional,
        has_overrides_with_less_direct_parameters=has_overrides,
    )


def _parse_elf64(data: bytes) -> ElfIdentity:
    header_fmt = "<16sHHIQQQIHHHHHH"
    header_size = struct.calcsize(header_fmt)
    if len(data) < header_size:
        raise ValueError("libapp.so is too small for an ELF64 header")
    header = struct.unpack_from(header_fmt, data, 0)
    ident = header[0]
    if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1:
        raise ValueError("compiler sidecar requires little-endian ELF64 libapp.so")
    e_machine = header[2]
    if e_machine != 183:  # EM_AARCH64
        raise ValueError("compiler sidecar currently supports ARM64 libapp.so only")
    phoff = header[5]
    phentsize = header[9]
    phnum = header[10]
    ph_fmt = "<IIQQQQQQ"
    if phentsize != struct.calcsize(ph_fmt):
        raise ValueError("unexpected ELF64 program-header size")

    segments: list[LoadSegment] = []
    build_id = ""
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + phentsize > len(data):
            raise ValueError("ELF64 program-header table is truncated")
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from(
            ph_fmt, data, offset
        )
        if p_type == 1:  # PT_LOAD
            segments.append(LoadSegment(p_offset, p_vaddr, p_filesz, p_flags))
        elif p_type == 4:  # PT_NOTE
            note_end = min(len(data), p_offset + p_filesz)
            cursor = p_offset
            while cursor + 12 <= note_end:
                namesz, descsz, note_type = struct.unpack_from("<III", data, cursor)
                cursor += 12
                name_start = cursor
                name_padded = (namesz + 3) & ~3
                desc_start = name_start + name_padded
                desc_padded = (descsz + 3) & ~3
                next_cursor = desc_start + desc_padded
                if next_cursor > note_end:
                    break
                if note_type == 3 and data[name_start : name_start + min(namesz, 3)] == b"GNU":
                    build_id = data[desc_start : desc_start + descsz].hex()
                cursor = next_cursor

    hashes: set[str] = set()
    cursor = 0
    while True:
        cursor = data.find(_SNAPSHOT_MAGIC, cursor)
        if cursor < 0:
            break
        raw = data[cursor + 20 : cursor + 52]
        if len(raw) == 32 and _HEX_32_RE.fullmatch(raw):
            hashes.add(raw.decode("ascii").lower())
        cursor += 1
    if not build_id:
        raise ValueError("libapp.so has no GNU build-id")
    if len(hashes) != 1:
        raise ValueError(f"expected one Dart snapshot hash, found {sorted(hashes)}")
    return ElfIdentity(build_id, next(iter(hashes)), tuple(segments))


def _extract_machine_code(log: str, function_name: str) -> bytes:
    # Current Dart printer prefixes private/top-level names with an underscore,
    # producing ..._::_verifiedAbiDouble. Accept both forms so this producer is
    # not tied to that cosmetic separator.
    suffixes = (f"_::{function_name}", f"_::_{function_name}")
    collecting = False
    words: list[int] = []
    matched_name = ""
    for line in log.splitlines():
        if not collecting:
            match = _CODE_HEADER_RE.match(line)
            if match is None:
                continue
            candidate = match.group("name")
            if not any(candidate.endswith(suffix) for suffix in suffixes):
                continue
            collecting = True
            matched_name = candidate
            continue
        if line.strip() == "}":
            break
        match = _INSTRUCTION_RE.match(line)
        if match is not None:
            words.append(int(match.group("word"), 16))
    if not collecting or not words:
        raise ValueError(f"gen_snapshot did not disassemble RegularFunction {function_name!r}")
    code = b"".join(struct.pack("<I", word) for word in words)
    if not code:
        raise ValueError(f"empty machine code for {matched_name}")
    return code


def _representation_from_text(text: str) -> str:
    stripped = text.strip()
    if stripped.endswith(" double") or stripped == "double":
        return "unboxed-double"
    if stripped.endswith(" int64") or stripped == "int64":
        return "unboxed-int64"
    if "T{" in stripped or stripped.startswith("Box(") or stripped.startswith("Constant("):
        return "tagged"
    return "unknown"


def _extract_abi(log: str, function_name: str) -> AbiEvidence:
    suffixes = (f"_::{function_name}", f"_::_{function_name}")
    in_function = False
    parameters: dict[int, tuple[str, str]] = {}
    values: dict[int, str] = {}
    result = "unknown"
    for line in log.splitlines():
        if not in_function:
            match = _CFG_HEADER_RE.match(line)
            if match is None or not any(
                match.group("name").endswith(suffix) for suffix in suffixes
            ):
                continue
            in_function = True
            continue
        if line == "*** END CFG":
            break

        parameter = _PARAMETER_RE.match(line)
        if parameter is not None:
            index = int(parameter.group("index"))
            location = parameter.group("location")
            representation = _representation_from_text(parameter.group("tail"))
            # Boxed/tagged parameters are printed with only a CompileType
            # (T{...}). Register-allocated unboxed parameters carry an explicit
            # machine representation such as `double` or `int64`.
            if representation == "unknown" and "T{" in parameter.group("tail"):
                representation = "tagged"
            parameters[index] = (representation, location)
            values[int(parameter.group("value"))] = representation
            continue

        value_def = _VALUE_DEF_RE.match(line)
        if value_def is not None:
            values[int(value_def.group("value"))] = _representation_from_text(
                value_def.group("body")
            )
        returned = _RETURN_RE.match(line)
        if returned is not None:
            result = values.get(int(returned.group("value")), "unknown")

    if not in_function or not parameters:
        raise ValueError(f"optimized CFG did not expose parameters for {function_name!r}")
    ordered_indexes = sorted(parameters)
    if ordered_indexes != list(range(len(ordered_indexes))):
        raise ValueError(f"non-contiguous formal parameter indexes: {ordered_indexes}")
    representations = tuple(parameters[index][0] for index in ordered_indexes)
    if "unknown" in representations or result == "unknown":
        raise ValueError(
            f"compiler CFG did not prove a complete ABI for {function_name!r}: "
            f"parameters={representations} result={result}"
        )
    locations = [parameters[index][1] for index in ordered_indexes]
    must_use_stack = bool(locations) and all(location.startswith("fp[") for location in locations)
    max_in_registers = 0 if must_use_stack else len(representations)
    return AbiEvidence(
        parameters=representations,
        result=result,
        max_parameters_in_registers=max_in_registers,
        must_use_stack_calling_convention=must_use_stack,
    )


def _find_unique_executable_va(data: bytes, identity: ElfIdentity, code: bytes) -> int:
    matches: list[int] = []
    for segment in identity.load_segments:
        if segment.flags & 0x1 == 0:  # PF_X
            continue
        start = segment.file_offset
        end = min(len(data), start + segment.file_size)
        cursor = start
        while True:
            found = data.find(code, cursor, end)
            if found < 0:
                break
            matches.append(segment.virtual_address + (found - segment.file_offset))
            cursor = found + 1
    if len(matches) != 1:
        raise ValueError(
            f"expected one executable match for compiler code bytes, found "
            f"{[hex(value) for value in matches]}"
        )
    return matches[0]


def _fnv1a64(data: bytes) -> str:
    value = 14695981039346656037
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def _extract_code_identity_profile(
    profile_path: Path,
    library_uri: str,
    class_name: str,
    function_name: str,
) -> CodeIdentityEvidence:
    profile = json.loads(profile_path.read_text())
    meta = profile["snapshot"]["meta"]
    node_fields = meta["node_fields"]
    edge_fields = meta["edge_fields"]
    node_types = meta["node_types"][0]
    edge_types = meta["edge_types"][0]
    strings = profile["strings"]
    nodes = profile["nodes"]
    edges = profile["edges"]
    node_width = len(node_fields)
    edge_width = len(edge_fields)
    node_field = {name: index for index, name in enumerate(node_fields)}
    edge_field = {name: index for index, name in enumerate(edge_fields)}
    node_count = len(nodes) // node_width

    names: list[str] = []
    types: list[str] = []
    edge_counts: list[int] = []
    for index in range(node_count):
        base = index * node_width
        names.append(strings[nodes[base + node_field["name"]]])
        types.append(node_types[nodes[base + node_field["type"]]])
        edge_counts.append(nodes[base + node_field["edge_count"]])

    function_owner: dict[int, int] = {}
    function_code: dict[int, int] = {}
    class_library: dict[int, int] = {}
    code_functions: dict[int, list[int]] = {}
    edge_position = 0
    for source, count in enumerate(edge_counts):
        for _ in range(count):
            base = edge_position * edge_width
            edge_type = edge_types[edges[base + edge_field["type"]]]
            name_value = edges[base + edge_field["name_or_index"]]
            edge_name = (
                strings[name_value]
                if edge_type in ("context", "property", "internal")
                else ""
            )
            target = edges[base + edge_field["to_node"]] // node_width
            if types[source] == "Function":
                if edge_name == "owner_":
                    function_owner[source] = target
                elif edge_name == "code_":
                    function_code[source] = target
                    code_functions.setdefault(target, []).append(source)
            elif types[source] == "Class" and edge_name == "library_":
                class_library[source] = target
            edge_position += 1

    profile_class_name = "::" if class_name == "Global" else class_name
    candidates: list[tuple[int, int]] = []
    for function, code in function_code.items():
        if names[function] != function_name:
            continue
        owner = function_owner.get(function)
        if owner is None or types[owner] != "Class" or names[owner] != profile_class_name:
            continue
        library = class_library.get(owner)
        if library is None or types[library] != "Library" or names[library] != library_uri:
            continue
        candidates.append((function, code))

    if len(candidates) != 1:
        raise ValueError(
            "snapshot profile did not identify exactly one compiler Function for "
            f"{library_uri}/{class_name}/{function_name}: {len(candidates)} candidates"
        )
    _, code = candidates[0]
    aliases = code_functions.get(code, [])
    if not aliases:
        raise ValueError("snapshot profile target Code has no Function.code_ references")
    alias_count = len(aliases)
    return CodeIdentityEvidence(
        proof="unique" if alias_count == 1 else "shared",
        physical_entry_alias_count=alias_count,
    )


def _run_gen_snapshot(
    gen_snapshot: Path,
    dill: Path,
    library_uri: str,
    class_name: str,
    function_name: str,
) -> tuple[str, CodeIdentityEvidence]:
    with tempfile.TemporaryDirectory(prefix="dartplant-oracle-") as temp_dir:
        elf = Path(temp_dir) / "oracle.so"
        profile = Path(temp_dir) / "oracle.heapsnapshot"
        command = [
            str(gen_snapshot),
            "--deterministic",
            "--snapshot_kind=app-aot-elf",
            f"--elf={elf}",
            "--strip",
            "--disassemble",
            "--print-flow-graph",
            "--print-flow-graph-optimized",
            f"--print-flow-graph-filter={function_name}",
            f"--write-v8-snapshot-profile-to={profile}",
            str(dill),
        ]
        result = sp.run(command, check=False, stdout=sp.PIPE, stderr=sp.STDOUT, text=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"gen_snapshot oracle failed with exit code {result.returncode}\n{result.stdout}"
            )
        return result.stdout, _extract_code_identity_profile(
            profile, library_uri, class_name, function_name
        )


def _write_header(path: Path, record: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = f"""// Generated by tools/compiler-oracle/build_snapshot_sidecar.py.
#pragma once

#include \"dartplant/advanced/artifact.h\"

#define DARTPLANT_ORDINARY_AOT_SIDECAR_AVAILABLE 1

inline constexpr DartPlantSnapshotFunctionInfo kDartPlantOrdinaryAotFunctions[] = {{{{
    .struct_size = sizeof(DartPlantSnapshotFunctionInfo),
    .library_uri = {json.dumps(record['library_uri'])},
    .class_name = {json.dumps(record['class_name'])},
    .function_name = {json.dumps(record['function_name'])},
    .signature = \"\",
    .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    .entry_va = 0x{record['entry_va']:x}ULL,
    .code_size = {record['code_size']}ULL,
    .code_section_va = 0,
    .fingerprint = {json.dumps(record['fingerprint'])},
    .code_identity_proof = DARTPLANT_CODE_IDENTITY_{str(record['code_identity_proof']).upper()},
    .physical_entry_alias_count = {record['physical_entry_alias_count']},
}}}};

inline constexpr DartPlantSnapshotIndexInfo kDartPlantOrdinaryAotSnapshotIndex = {{
    .struct_size = sizeof(DartPlantSnapshotIndexInfo),
    .module_name = \"libapp.so\",
    .module_build_id = {json.dumps(record['build_id'])},
    .snapshot_hash = {json.dumps(record['snapshot_hash'])},
    .dart_version = \"compiler-oracle\",
    .profile_version = \"compiler-oracle-v1\",
    .functions = kDartPlantOrdinaryAotFunctions,
    .function_count = 1,
}};

inline constexpr DartPlantAbiRepresentation kDartPlantOrdinaryAotParameters[] = {{
{chr(10).join(f'    DARTPLANT_ABI_REPRESENTATION_{str(value).upper().replace("-", "_")},' for value in record['abi_parameters'])}
}};

inline constexpr DartPlantCompilerAbiEvidence kDartPlantOrdinaryAotAbiEvidence = {{
    .struct_size = sizeof(DartPlantCompilerAbiEvidence),
    .snapshot_hash = {json.dumps(record['snapshot_hash'])},
    .app_build_id = {json.dumps(record['build_id'])},
    .code_fingerprint = {json.dumps(record['fingerprint'])},
    .parameter_representations = kDartPlantOrdinaryAotParameters,
    .parameter_count = {len(record['abi_parameters'])},
    .result_representation = DARTPLANT_ABI_REPRESENTATION_{str(record['abi_result']).upper().replace('-', '_')},
    .max_parameters_in_registers = {record['max_parameters_in_registers']},
    .must_use_stack_calling_convention = {1 if record['must_use_stack_calling_convention'] else 0},
    .has_optional_parameters = {1 if record['has_optional_parameters'] else 0},
    .has_overrides_with_less_direct_parameters = {1 if record['has_overrides_with_less_direct_parameters'] else 0},
    .reserved = 0,
    .library_uri = {json.dumps(record['library_uri'])},
    .class_name = {json.dumps(record['class_name'])},
    .function_name = {json.dumps(record['function_name'])},
    .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    .entry_va = 0x{record['entry_va']:x}ULL,
    .code_size = {record['code_size']}ULL,
}};

inline constexpr DartPlantArtifactBundle kDartPlantOrdinaryAotArtifactBundle = {{
    .struct_size = sizeof(DartPlantArtifactBundle),
    .version = DARTPLANT_ARTIFACT_BUNDLE_VERSION,
    .snapshot_index = &kDartPlantOrdinaryAotSnapshotIndex,
    .compiler_abi_evidence = &kDartPlantOrdinaryAotAbiEvidence,
    .compiler_abi_evidence_count = 1,
}};

#if defined(__cplusplus)
namespace dartplant_generated {{
struct OrdinaryAotArtifactRegistrar {{
    OrdinaryAotArtifactRegistrar() {{
        (void)dartplant_register_embedded_artifact_bundle(&kDartPlantOrdinaryAotArtifactBundle);
    }}
}};
inline const OrdinaryAotArtifactRegistrar kOrdinaryAotArtifactRegistrar{{}};
}}  // namespace dartplant_generated
#endif
"""
    path.write_text(text)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build an exact DartPlant snapshot sidecar record from compiler diagnostics"
    )
    parser.add_argument("--gen-snapshot", type=Path, required=True)
    parser.add_argument("--dill", type=Path, required=True)
    parser.add_argument("--libapp", type=Path, required=True)
    parser.add_argument("--library-uri", required=True)
    parser.add_argument("--class-name", required=True)
    parser.add_argument("--function-name", required=True)
    parser.add_argument("--abi-oracle-json", type=Path, required=True)
    parser.add_argument("--output-header", type=Path, required=True)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()

    libapp = args.libapp.read_bytes()
    identity = _parse_elf64(libapp)
    log, code_identity = _run_gen_snapshot(
        args.gen_snapshot,
        args.dill,
        args.library_uri,
        args.class_name,
        args.function_name,
    )
    code = _extract_machine_code(log, args.function_name)
    abi = _load_abi_oracle(
        args.abi_oracle_json,
        args.library_uri,
        args.class_name,
        args.function_name,
    )
    # gen_snapshot's human-readable CFG is intentionally only an independent
    # cross-check. Runtime ABI evidence is emitted from vm.unboxing-info.metadata
    # above; changing printer text cannot silently become compiler truth.
    cfg_abi = _extract_abi(log, args.function_name)
    if cfg_abi.parameters != abi.parameters or cfg_abi.result != abi.result:
        raise ValueError(
            "compiler ABI oracle disagrees with optimized CFG cross-check: "
            f"oracle={abi.parameters}->{abi.result} "
            f"cfg={cfg_abi.parameters}->{cfg_abi.result}"
        )
    entry_va = _find_unique_executable_va(libapp, identity, code)
    record: dict[str, object] = {
        "library_uri": args.library_uri,
        "class_name": args.class_name,
        "function_name": args.function_name,
        "entry_va": entry_va,
        "code_size": len(code),
        "fingerprint": _fnv1a64(code),
        "build_id": identity.build_id,
        "snapshot_hash": identity.snapshot_hash,
        "code_identity_proof": code_identity.proof,
        "physical_entry_alias_count": code_identity.physical_entry_alias_count,
        "abi_parameters": list(abi.parameters),
        "abi_result": abi.result,
        "max_parameters_in_registers": abi.max_parameters_in_registers,
        "must_use_stack_calling_convention": abi.must_use_stack_calling_convention,
        "has_optional_parameters": abi.has_optional_parameters,
        "has_overrides_with_less_direct_parameters": (
            abi.has_overrides_with_less_direct_parameters
        ),
    }
    _write_header(args.output_header, record)
    if args.output_json is not None:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(record, indent=2) + "\n")
    print(
        f"compiler sidecar: {args.library_uri}/{args.class_name}/{args.function_name} "
        f"entry_va=0x{entry_va:x} size={len(code)} fingerprint={record['fingerprint']} "
        f"identity={code_identity.proof}/{code_identity.physical_entry_alias_count} "
        f"abi={abi.parameters}->{abi.result}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
