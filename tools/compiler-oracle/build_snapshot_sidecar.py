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
    r"^Code for optimized function '(?P<name>.+)' \((?P<kind>[A-Za-z]+Function)\) \{$"
)
_INSTRUCTION_RE = re.compile(
    r"^(?P<address>0x[0-9a-fA-F]+)\s+(?P<word>[0-9a-fA-F]{8})\s+"
)
_CFG_HEADER_RE = re.compile(
    r"^==== (?P<name>.+) \((?P<kind>[A-Za-z]+Function)\)$"
)
_ENTRY_POINTS_HEADER_RE = re.compile(r"^Entry points for function '(?P<name>.+)' \{$")
_ENTRY_POINT_RE = re.compile(
    r"^\s*\[code\+0x[0-9a-fA-F]+\]\s+(?P<address>[0-9a-fA-F]+)\s+"
    r"(?P<kind>kNormal|kUnchecked|kMonomorphic|kMonomorphicUnchecked)$"
)
_PARAMETER_RE = re.compile(
    r"^\s*v(?P<value>\d+) <- Parameter\((?P<index>\d+) @(?P<location>[^)]+)\)(?P<tail>.*)$"
)
_VALUE_DEF_RE = re.compile(r"^\s*(?:\d+:\s+)?v(?P<value>\d+) <- (?P<body>.+)$")
_RETURN_RE = re.compile(r"^\s*\d+:\s+DartReturn(?::\d+)?\(v(?P<value>\d+)\)")
_SNAPSHOT_MAGIC = b"\xf5\xf5\xdc\xdc"
_HEX_32_RE = re.compile(rb"^[0-9a-fA-F]{32}$")

_ENTRY_KIND_LABELS = {
    "default": "kNormal",
    "unchecked": "kUnchecked",
    "monomorphic": "kMonomorphic",
    "monomorphic-unchecked": "kMonomorphicUnchecked",
}
_ENTRY_KIND_CPP = {
    "default": "DARTPLANT_ENTRY_DEFAULT",
    "unchecked": "DARTPLANT_ENTRY_UNCHECKED",
    "monomorphic": "DARTPLANT_ENTRY_MONOMORPHIC",
    "monomorphic-unchecked": "DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED",
}
_FUNCTION_KIND_VALUES = {
    "RegularFunction": 0,
    "ClosureFunction": 1,
    "ImplicitClosureFunction": 2,
}


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
class MachineCodeEvidence:
    code: bytes
    start_address: int
    printed_name: str
    function_kind: str


@dataclass(frozen=True)
class AbiEvidence:
    parameters: tuple[str, ...]
    result: str
    max_parameters_in_registers: int
    must_use_stack_calling_convention: bool
    has_optional_parameters: bool = False
    has_overrides_with_less_direct_parameters: bool = False
    implicit_parameter_count: int = 0


@dataclass(frozen=True)
class CodeIdentityEvidence:
    proof: str
    physical_entry_alias_count: int


def _expected_register_representations(abi: AbiEvidence) -> dict[tuple[str, int], str]:
    gp_registers = (1, 2, 3, 5, 6, 7)
    fpu_registers = (0, 1, 2, 3, 4, 5)
    gp_index = 0
    fpu_index = 0
    expected: dict[tuple[str, int], str] = {}
    for index, representation in enumerate(abi.parameters):
        if index >= abi.max_parameters_in_registers:
            continue
        if representation in {"tagged", "unboxed-int64"}:
            if gp_index < len(gp_registers):
                expected[("gp", gp_registers[gp_index])] = representation
                gp_index += 1
        elif representation == "unboxed-double":
            if fpu_index < len(fpu_registers):
                expected[("fpu", fpu_registers[fpu_index])] = representation
                fpu_index += 1
    return expected


def _run_machine_code_analyzer(
    analyzer: Path,
    code: bytes,
    entry_va: int,
) -> dict[str, object]:
    if not analyzer.is_file():
        raise ValueError(f"ARM64 structural analyzer not found: {analyzer}")
    with tempfile.TemporaryDirectory(prefix="dartplant-aot-analysis-") as temp_dir:
        code_path = Path(temp_dir) / "code.bin"
        code_path.write_bytes(code)
        result = sp.run(
            [
                str(analyzer),
                "--code-file",
                str(code_path),
                "--address",
                hex(entry_va),
            ],
            check=False,
            stdout=sp.PIPE,
            stderr=sp.PIPE,
            text=True,
        )
    if result.returncode != 0:
        raise ValueError(
            "ARM64 structural analyzer failed: "
            f"exit={result.returncode} stderr={result.stderr.strip()}"
        )
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise ValueError("ARM64 structural analyzer emitted invalid JSON") from error
    if not isinstance(document, dict):
        raise ValueError("ARM64 structural analyzer result must be an object")
    if document.get("schema_version") != 1:
        raise ValueError("unsupported ARM64 structural analyzer schema")
    if document.get("structural_evidence_truncated") is True:
        raise ValueError("ARM64 structural analyzer evidence exceeded its fixed proof capacity")
    return document


def _validate_machine_code_cross_check(
    abi: AbiEvidence,
    document: dict[str, object],
) -> None:
    observations = document.get("observations")
    if not isinstance(observations, list):
        raise ValueError("ARM64 structural analyzer result has no observations array")
    expected = _expected_register_representations(abi)
    for observation in observations:
        if not isinstance(observation, dict):
            raise ValueError("ARM64 structural analyzer observation is invalid")
        location = observation.get("location")
        representation = observation.get("representation")
        register = observation.get("register")
        if representation == "unknown" or location == "entry-stack":
            continue
        if location not in {"gp", "fpu"} or not isinstance(register, int):
            continue
        expected_representation = expected.get((location, register))
        if expected_representation is None:
            raise ValueError(
                "machine-code structural analysis observed an ABI value in an "
                f"unassigned {location}{register}: {representation}"
            )
        if representation != expected_representation:
            raise ValueError(
                "compiler ABI oracle disagrees with machine-code structural analysis at "
                f"{location}{register}: oracle={expected_representation} "
                f"machine={representation}"
            )


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
    implicit_parameter_count = function.get("implicit_parameter_count", 0)
    if not all(isinstance(value, bool) for value in (must_use_stack, has_optional, has_overrides)):
        raise ValueError("ABI oracle calling-convention flags are invalid")
    if (
        not isinstance(implicit_parameter_count, int)
        or implicit_parameter_count < 0
        or implicit_parameter_count > fixed_parameter_count
    ):
        raise ValueError("ABI oracle implicit-parameter count is invalid")
    if must_use_stack and max_parameters_in_registers != 0:
        raise ValueError("ABI oracle forced-stack Function still reports register parameters")

    return AbiEvidence(
        parameters=tuple(parameters),
        result=result,
        max_parameters_in_registers=max_parameters_in_registers,
        must_use_stack_calling_convention=must_use_stack,
        has_optional_parameters=has_optional,
        has_overrides_with_less_direct_parameters=has_overrides,
        implicit_parameter_count=implicit_parameter_count,
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


def _extract_machine_code(
    log: str, function_name: str, expected_kind: str | None = None
) -> MachineCodeEvidence:
    # Current Dart printer prefixes private/top-level names with an underscore.
    # Ordinary functions can be selected by their exact suffix. A closure
    # artifact additionally supplies the compiler Function kind; in that mode
    # require exactly one disassembly of that kind whose printed identity
    # contains the source member name instead of guessing a register/opcode
    # signature.
    suffixes = (f"_::{function_name}", f"_::_{function_name}")
    lines = log.splitlines()
    matches: list[MachineCodeEvidence] = []
    position = 0
    while position < len(lines):
        header = _CODE_HEADER_RE.match(lines[position])
        if header is None:
            position += 1
            continue
        candidate = header.group("name")
        kind = header.group("kind")
        ordinary_match = any(candidate.endswith(suffix) for suffix in suffixes)
        kind_match = expected_kind is not None and kind == expected_kind and function_name in candidate
        position += 1
        words: list[int] = []
        start_address = 0
        while position < len(lines) and lines[position].strip() != "}":
            instruction = _INSTRUCTION_RE.match(lines[position])
            if instruction is not None:
                if start_address == 0:
                    start_address = int(instruction.group("address"), 16)
                words.append(int(instruction.group("word"), 16))
            position += 1
        if (expected_kind is None and ordinary_match) or kind_match:
            if not words or start_address == 0:
                raise ValueError(f"empty machine code for {candidate}")
            matches.append(
                MachineCodeEvidence(
                    b"".join(struct.pack("<I", word) for word in words),
                    start_address,
                    candidate,
                    kind,
                )
            )
        position += 1
    if len(matches) != 1:
        raise ValueError(
            "gen_snapshot did not identify exactly one compiler Function for "
            f"{function_name!r} kind={expected_kind or 'ordinary'}: {len(matches)}"
        )
    return matches[0]


def _extract_entry_points(log: str, printed_name: str) -> dict[str, int]:
    collecting = False
    entries: dict[str, int] = {}
    for line in log.splitlines():
        if not collecting:
            match = _ENTRY_POINTS_HEADER_RE.match(line)
            if match is None or match.group("name") != printed_name:
                continue
            collecting = True
            continue
        if line.strip() == "}":
            break
        match = _ENTRY_POINT_RE.match(line)
        if match is None:
            continue
        kind = match.group("kind")
        if kind in entries:
            raise ValueError(f"duplicate compiler entry-point record for {kind}")
        entries[kind] = int(match.group("address"), 16)
    required = {"kNormal", "kUnchecked", "kMonomorphic", "kMonomorphicUnchecked"}
    if set(entries) != required:
        raise ValueError(
            f"compiler entry-point family for {printed_name!r} is incomplete: {sorted(entries)}"
        )
    return entries


def _representation_from_text(text: str) -> str:
    stripped = text.strip()
    if stripped.endswith(" double") or stripped == "double":
        return "unboxed-double"
    if stripped.endswith(" int64") or stripped == "int64":
        return "unboxed-int64"
    if "T{" in stripped or stripped.startswith("Box(") or stripped.startswith("Constant("):
        return "tagged"
    return "unknown"


def _extract_abi(
    log: str, function_name: str, *, exact_printed_name: str | None = None
) -> AbiEvidence:
    suffixes = (f"_::{function_name}", f"_::_{function_name}")
    in_function = False
    parameters: dict[int, tuple[str, str]] = {}
    values: dict[int, str] = {}
    result = "unknown"
    for line in log.splitlines():
        if not in_function:
            match = _CFG_HEADER_RE.match(line)
            if match is None:
                continue
            if exact_printed_name is not None:
                if match.group("name") != exact_printed_name:
                    continue
            elif not any(match.group("name").endswith(suffix) for suffix in suffixes):
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
    locations = [parameters[index][1] for index in ordered_indexes]
    must_use_stack = bool(locations) and all(location.startswith("fp[") for location in locations)
    max_in_registers = 0 if must_use_stack else len(representations)
    return AbiEvidence(
        parameters=representations,
        result=result,
        max_parameters_in_registers=max_in_registers,
        must_use_stack_calling_convention=must_use_stack,
    )


def _derive_closure_abi_from_cfg(
    parent: AbiEvidence,
    closure_cfg: AbiEvidence,
) -> AbiEvidence:
    """Derive the synthetic closure call ABI from exact compiler CFG facts.

    vm.unboxing-info.metadata is attached to Kernel members, not to the
    synthetic ClosureFunction/ImplicitClosureFunction object emitted by the VM.
    Dart source gives that family a dedicated call contract: the Closure object
    is hidden from the user formal list, MaxNumberOfParametersInRegisters()
    returns zero, and the synthetic Function starts with boxed parameter/return
    state. We therefore require the selected synthetic CFG to prove exactly the
    matching boxed stack shape and strip only its leading hidden Closure formal.
    """
    if not closure_cfg.parameters:
        raise ValueError("compiler closure CFG has no hidden Closure parameter")
    if not closure_cfg.must_use_stack_calling_convention or closure_cfg.max_parameters_in_registers != 0:
        raise ValueError("compiler closure CFG does not use the required forced-stack convention")
    if any(parameter != "tagged" for parameter in closure_cfg.parameters):
        raise ValueError(
            "compiler closure CFG contradicts the boxed synthetic-closure parameter contract"
        )
    if closure_cfg.result != "tagged":
        raise ValueError(
            "compiler closure CFG contradicts the boxed synthetic-closure result contract"
        )
    user_parameter_count = len(parent.parameters) - parent.implicit_parameter_count
    if user_parameter_count < 0 or len(closure_cfg.parameters) != user_parameter_count + 1:
        raise ValueError(
            "compiler closure CFG formal count disagrees with the parent Function signature: "
            f"parent_user={user_parameter_count} closure={len(closure_cfg.parameters)}"
        )
    return AbiEvidence(
        parameters=closure_cfg.parameters[1:],
        result=closure_cfg.result,
        max_parameters_in_registers=0,
        must_use_stack_calling_convention=True,
        has_optional_parameters=parent.has_optional_parameters,
        has_overrides_with_less_direct_parameters=False,
        implicit_parameter_count=0,
    )


def _validate_cfg_cross_check(oracle: AbiEvidence, cfg: AbiEvidence) -> None:
    """Reject CFG facts that contradict compiler metadata.

    vm.unboxing-info.metadata is the exact compiler-side ABI source of truth.
    The optimized CFG printer is only an independent cross-check and is not
    guaranteed to print a machine representation for every boxed/record return.
    Unknown CFG slots therefore carry no proof; any slot it *does* prove must
    agree exactly with the metadata oracle.
    """
    if len(cfg.parameters) != len(oracle.parameters):
        raise ValueError(
            "compiler ABI oracle disagrees with optimized CFG parameter count: "
            f"oracle={len(oracle.parameters)} cfg={len(cfg.parameters)}"
        )
    for index, (oracle_rep, cfg_rep) in enumerate(zip(oracle.parameters, cfg.parameters)):
        if cfg_rep != "unknown" and cfg_rep != oracle_rep:
            raise ValueError(
                "compiler ABI oracle disagrees with optimized CFG parameter "
                f"{index}: oracle={oracle_rep} cfg={cfg_rep}"
            )
    if cfg.result != "unknown" and cfg.result != oracle.result:
        raise ValueError(
            "compiler ABI oracle disagrees with optimized CFG result: "
            f"oracle={oracle.result} cfg={cfg.result}"
        )
    if cfg.must_use_stack_calling_convention != oracle.must_use_stack_calling_convention:
        raise ValueError(
            "compiler ABI oracle disagrees with optimized CFG calling convention: "
            f"oracle_stack={oracle.must_use_stack_calling_convention} "
            f"cfg_stack={cfg.must_use_stack_calling_convention}"
        )
    if cfg.max_parameters_in_registers != oracle.max_parameters_in_registers:
        raise ValueError(
            "compiler ABI oracle disagrees with optimized CFG register parameter limit: "
            f"oracle={oracle.max_parameters_in_registers} "
            f"cfg={cfg.max_parameters_in_registers}"
        )


def _normalize_aarch64_direct_branches(code: bytes) -> bytes:
    if len(code) % 4 != 0:
        raise ValueError("AArch64 compiler code size is not instruction aligned")
    normalized = bytearray(code)
    for offset in range(0, len(code), 4):
        word = struct.unpack_from("<I", code, offset)[0]
        opcode = word & 0xFC000000
        # AArch64 B/BL encode a signed imm26 PC-relative displacement. The
        # displacement to an out-of-line VM stub can legitimately change when
        # gen_snapshot is re-run with diagnostic flags even though the target
        # Function body and calling convention are identical. A branch whose
        # decoded target remains inside this Function is real intra-Function
        # control flow and must stay part of the machine-code identity. Mask
        # only branches whose target is outside [0, len(code)); those are the
        # relocation-like calls/jumps this fallback is intended to tolerate.
        # The emitted runtime fingerprint is still taken from the exact bytes
        # in the target Flutter libapp.so.
        if opcode in (0x14000000, 0x94000000):
            imm26 = word & 0x03FFFFFF
            if imm26 & 0x02000000:
                imm26 -= 1 << 26
            target_offset = offset + (imm26 << 2)
            if target_offset < 0 or target_offset >= len(code):
                struct.pack_into("<I", normalized, offset, opcode)
    return bytes(normalized)


def _aarch64_has_direct_call_to(code: bytes, entry_va: int, target_va: int) -> bool:
    if len(code) % 4 != 0:
        return False
    for offset in range(0, len(code), 4):
        word = struct.unpack_from("<I", code, offset)[0]
        if word & 0xFC000000 != 0x94000000:  # BL imm26
            continue
        imm26 = word & 0x03FFFFFF
        if imm26 & 0x02000000:
            imm26 -= 1 << 26
        if entry_va + offset + (imm26 << 2) == target_va:
            return True
    return False


def _find_unique_executable_code(
    data: bytes,
    identity: ElfIdentity,
    code: bytes,
    required_direct_call_target_va: int | None = None,
) -> tuple[int, bytes, str]:
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
    if required_direct_call_target_va is not None and matches:
        filtered: list[int] = []
        for entry_va in matches:
            for segment in identity.load_segments:
                if segment.virtual_address <= entry_va < segment.virtual_address + segment.file_size:
                    file_offset = segment.file_offset + (entry_va - segment.virtual_address)
                    candidate = data[file_offset : file_offset + len(code)]
                    if _aarch64_has_direct_call_to(
                        candidate, entry_va, required_direct_call_target_va
                    ):
                        filtered.append(entry_va)
                    break
        matches = filtered
    if len(matches) == 1:
        entry_va = matches[0]
        for segment in identity.load_segments:
            if segment.virtual_address <= entry_va < segment.virtual_address + segment.file_size:
                file_offset = segment.file_offset + (entry_va - segment.virtual_address)
                return entry_va, data[file_offset : file_offset + len(code)], "exact+relation" if required_direct_call_target_va is not None else "exact"
        raise ValueError("exact executable code match could not be mapped back to file bytes")
    if len(matches) > 1:
        raise ValueError(
            f"expected one executable match for compiler code bytes, found "
            f"{[hex(value) for value in matches]}"
        )

    normalized_code = _normalize_aarch64_direct_branches(code)
    normalized_matches: list[tuple[int, bytes]] = []
    for segment in identity.load_segments:
        if segment.flags & 0x1 == 0:  # PF_X
            continue
        start = segment.file_offset
        end = min(len(data), start + segment.file_size)
        limit = end - len(code)
        for file_offset in range(start, limit + 1, 4):
            candidate = data[file_offset : file_offset + len(code)]
            if _normalize_aarch64_direct_branches(candidate) != normalized_code:
                continue
            entry_va = segment.virtual_address + (file_offset - segment.file_offset)
            normalized_matches.append((entry_va, candidate))
    if required_direct_call_target_va is not None:
        normalized_matches = [
            (entry_va, candidate)
            for entry_va, candidate in normalized_matches
            if _aarch64_has_direct_call_to(
                candidate, entry_va, required_direct_call_target_va
            )
        ]
    if len(normalized_matches) != 1:
        relation = (
            f" with required BL target 0x{required_direct_call_target_va:x}"
            if required_direct_call_target_va is not None
            else ""
        )
        raise ValueError(
            "expected one executable match after masking only AArch64 B/BL imm26 fields"
            f"{relation}, found {[hex(value) for value, _ in normalized_matches]}"
        )
    entry_va, actual_code = normalized_matches[0]
    mode = (
        "branch-relocation-normalized+direct-call-relation"
        if required_direct_call_target_va is not None
        else "branch-relocation-normalized"
    )
    return entry_va, actual_code, mode


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
    artifact_function_name: str | None = None,
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
            profile, library_uri, class_name, artifact_function_name or function_name
        )


def _write_header(
    path: Path, record: dict[str, object], symbol_prefix: str = "DartPlantOrdinaryAot"
) -> None:
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol_prefix) is None:
        raise ValueError(f"invalid generated C++ symbol prefix: {symbol_prefix!r}")
    path.parent.mkdir(parents=True, exist_ok=True)
    functions_symbol = f"k{symbol_prefix}Functions"
    snapshot_symbol = f"k{symbol_prefix}SnapshotIndex"
    parameters_symbol = f"k{symbol_prefix}Parameters"
    evidence_symbol = f"k{symbol_prefix}AbiEvidence"
    bundle_symbol = f"k{symbol_prefix}ArtifactBundle"
    registrar_symbol = f"{symbol_prefix}ArtifactRegistrar"
    registrar_instance = f"k{symbol_prefix}ArtifactRegistrar"
    entry_kind = str(record.get("entry_kind", "default"))
    if entry_kind not in _ENTRY_KIND_CPP:
        raise ValueError(f"unsupported generated entry kind: {entry_kind!r}")
    function_kind = int(record.get("function_kind", 0))
    closure_call_entry_only = bool(record.get("closure_call_entry_only", False))
    code_payload_va = int(record.get("code_payload_va", record["entry_va"]))
    code_instructions_length = int(
        record.get("code_instructions_length", record["code_size"])
    )
    compatibility_macro = (
        "#define DARTPLANT_ORDINARY_AOT_SIDECAR_AVAILABLE 1\n"
        if symbol_prefix == "DartPlantOrdinaryAot"
        else ""
    )
    structural = record.get("structural_analysis")
    if isinstance(structural, dict):
        structural_schema_version = 1
        structural_verified = 1
        structural_decoded_instructions = int(structural.get("decoded_instructions", 0))
        structural_basic_block_count = int(structural.get("basic_block_count", 0))
        structural_relation_count = sum(
            int(structural.get(name, 0))
            for name in (
                "external_branch_count",
                "indirect_call_count",
                "indirect_branch_count",
                "direct_call_count",
                "return_site_count",
                "address_materialization_count",
            )
        ) + (1 if structural.get("uses_arguments_descriptor", False) else 0)
        structural_unknown_control_flow = (
            1 if structural.get("has_unknown_control_flow", False) else 0
        )
        structural_uses_arguments_descriptor = (
            1 if structural.get("uses_arguments_descriptor", False) else 0
        )
        structural_reached_return = 1 if structural.get("reached_return", False) else 0
    else:
        structural_schema_version = 0
        structural_verified = 0
        structural_decoded_instructions = 0
        structural_basic_block_count = 0
        structural_relation_count = 0
        structural_unknown_control_flow = 0
        structural_uses_arguments_descriptor = 0
        structural_reached_return = 0
    text = f"""// Generated by tools/compiler-oracle/build_snapshot_sidecar.py.
#pragma once

#include \"dartplant/advanced/artifact.h\"

{compatibility_macro}

inline constexpr DartPlantSnapshotFunctionInfo {functions_symbol}[] = {{{{
    .struct_size = sizeof(DartPlantSnapshotFunctionInfo),
    .library_uri = {json.dumps(record['library_uri'])},
    .class_name = {json.dumps(record['class_name'])},
    .function_name = {json.dumps(record['function_name'])},
    .signature = \"\",
    .entry_kind = {_ENTRY_KIND_CPP[entry_kind]},
    .entry_va = 0x{record['entry_va']:x}ULL,
    .code_size = {record['code_size']}ULL,
    .code_section_va = 0,
    .fingerprint = {json.dumps(record['fingerprint'])},
    .code_identity_proof = DARTPLANT_CODE_IDENTITY_{str(record['code_identity_proof']).upper()},
    .physical_entry_alias_count = {record['physical_entry_alias_count']},
    .function_kind = {function_kind}u,
    .closure_call_entry_only = {1 if closure_call_entry_only else 0},
    .reserved_function_flags = {{0, 0, 0}},
    .code_payload_va = 0x{code_payload_va:x}ULL,
    .code_instructions_length = {code_instructions_length}ULL,
}}}};

inline constexpr DartPlantSnapshotIndexInfo {snapshot_symbol} = {{
    .struct_size = sizeof(DartPlantSnapshotIndexInfo),
    .module_name = \"libapp.so\",
    .module_build_id = {json.dumps(record['build_id'])},
    .snapshot_hash = {json.dumps(record['snapshot_hash'])},
    .dart_version = \"compiler-oracle\",
    .profile_version = \"compiler-oracle-v1\",
    .functions = {functions_symbol},
    .function_count = 1,
}};

inline constexpr DartPlantAbiRepresentation {parameters_symbol}[] = {{
{chr(10).join(f'    DARTPLANT_ABI_REPRESENTATION_{str(value).upper().replace("-", "_")},' for value in record['abi_parameters'])}
}};

inline constexpr DartPlantCompilerAbiEvidence {evidence_symbol} = {{
    .struct_size = sizeof(DartPlantCompilerAbiEvidence),
    .snapshot_hash = {json.dumps(record['snapshot_hash'])},
    .app_build_id = {json.dumps(record['build_id'])},
    .code_fingerprint = {json.dumps(record['fingerprint'])},
    .parameter_representations = {parameters_symbol},
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
    .entry_kind = {_ENTRY_KIND_CPP[entry_kind]},
    .entry_va = 0x{record['entry_va']:x}ULL,
    .code_size = {record['code_size']}ULL,
    .structural_schema_version = {structural_schema_version},
    .structural_decoded_instructions = {structural_decoded_instructions},
    .structural_basic_block_count = {structural_basic_block_count},
    .structural_relation_count = {structural_relation_count},
    .structural_verified = {structural_verified},
    .structural_has_unknown_control_flow = {structural_unknown_control_flow},
    .structural_uses_arguments_descriptor = {structural_uses_arguments_descriptor},
    .structural_reached_return = {structural_reached_return},
}};

inline constexpr DartPlantArtifactBundle {bundle_symbol} = {{
    .struct_size = sizeof(DartPlantArtifactBundle),
    .version = DARTPLANT_ARTIFACT_BUNDLE_VERSION,
    .snapshot_index = &{snapshot_symbol},
    .compiler_abi_evidence = &{evidence_symbol},
    .compiler_abi_evidence_count = 1,
}};

#if defined(__cplusplus)
namespace dartplant_generated {{
struct {registrar_symbol} {{
    {registrar_symbol}() {{
        (void)dartplant_register_embedded_artifact_bundle(&{bundle_symbol});
    }}
}};
inline const {registrar_symbol} {registrar_instance}{{}};
}}  // namespace dartplant_generated
#endif
"""
    path.write_text(text)



def _write_identity_header(
    path: Path, record: dict[str, object], symbol_prefix: str
) -> None:
    """Emit an exact Function/Code artifact without inventing typed ABI facts."""
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol_prefix) is None:
        raise ValueError(f"invalid generated C++ symbol prefix: {symbol_prefix!r}")
    path.parent.mkdir(parents=True, exist_ok=True)
    functions_symbol = f"k{symbol_prefix}Functions"
    snapshot_symbol = f"k{symbol_prefix}SnapshotIndex"
    bundle_symbol = f"k{symbol_prefix}ArtifactBundle"
    registrar_symbol = f"{symbol_prefix}ArtifactRegistrar"
    registrar_instance = f"k{symbol_prefix}ArtifactRegistrar"
    entry_kind = str(record.get("entry_kind", "default"))
    if entry_kind not in _ENTRY_KIND_CPP:
        raise ValueError(f"unsupported generated entry kind: {entry_kind!r}")
    function_kind = int(record.get("function_kind", 0))
    closure_call_entry_only = bool(record.get("closure_call_entry_only", False))
    code_payload_va = int(record.get("code_payload_va", record["entry_va"]))
    code_instructions_length = int(
        record.get("code_instructions_length", record["code_size"])
    )
    text = f"""// Generated by tools/compiler-oracle/build_snapshot_sidecar.py.
#pragma once

#include \"dartplant/advanced/artifact.h\"

inline constexpr DartPlantSnapshotFunctionInfo {functions_symbol}[] = {{{{
    .struct_size = sizeof(DartPlantSnapshotFunctionInfo),
    .library_uri = {json.dumps(record['library_uri'])},
    .class_name = {json.dumps(record['class_name'])},
    .function_name = {json.dumps(record['function_name'])},
    .signature = \"\",
    .entry_kind = {_ENTRY_KIND_CPP[entry_kind]},
    .entry_va = 0x{record['entry_va']:x}ULL,
    .code_size = {record['code_size']}ULL,
    .code_section_va = 0,
    .fingerprint = {json.dumps(record['fingerprint'])},
    .code_identity_proof = DARTPLANT_CODE_IDENTITY_{str(record['code_identity_proof']).upper()},
    .physical_entry_alias_count = {record['physical_entry_alias_count']},
    .function_kind = {function_kind}u,
    .closure_call_entry_only = {1 if closure_call_entry_only else 0},
    .reserved_function_flags = {{0, 0, 0}},
    .code_payload_va = 0x{code_payload_va:x}ULL,
    .code_instructions_length = {code_instructions_length}ULL,
}}}};

inline constexpr DartPlantSnapshotIndexInfo {snapshot_symbol} = {{
    .struct_size = sizeof(DartPlantSnapshotIndexInfo),
    .module_name = \"libapp.so\",
    .module_build_id = {json.dumps(record['build_id'])},
    .snapshot_hash = {json.dumps(record['snapshot_hash'])},
    .dart_version = \"compiler-oracle\",
    .profile_version = \"compiler-oracle-v1\",
    .functions = {functions_symbol},
    .function_count = 1,
}};

inline constexpr DartPlantArtifactBundle {bundle_symbol} = {{
    .struct_size = sizeof(DartPlantArtifactBundle),
    .version = DARTPLANT_ARTIFACT_BUNDLE_VERSION,
    .snapshot_index = &{snapshot_symbol},
    .compiler_abi_evidence = nullptr,
    .compiler_abi_evidence_count = 0,
}};

#if defined(__cplusplus)
namespace dartplant_generated {{
struct {registrar_symbol} {{
    {registrar_symbol}() {{
        (void)dartplant_register_embedded_artifact_bundle(&{bundle_symbol});
    }}
}};
inline const {registrar_symbol} {registrar_instance}{{}};
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
    parser.add_argument(
        "--artifact-function-name",
        help="exact heap-profile Function name when it differs from the source member name",
    )
    parser.add_argument(
        "--compiler-function-kind",
        choices=tuple(_FUNCTION_KIND_VALUES),
        help="require an exact compiler Function kind, e.g. ImplicitClosureFunction",
    )
    parser.add_argument(
        "--identity-only",
        action="store_true",
        help="emit exact snapshot/code identity without compiler typed ABI evidence",
    )
    parser.add_argument(
        "--entry-kind",
        choices=tuple(_ENTRY_KIND_LABELS),
        default="default",
        help="exact Dart CodeEntryKind to bind from compiler entry-point diagnostics",
    )
    parser.add_argument("--abi-oracle-json", type=Path, required=True)
    parser.add_argument(
        "--aot-analyzer",
        type=Path,
        help="optional DartPlant ARM64 machine-code structural analyzer",
    )
    parser.add_argument("--output-header", type=Path, required=True)
    parser.add_argument("--output-json", type=Path)
    parser.add_argument(
        "--symbol-prefix",
        default="DartPlantOrdinaryAot",
        help="unique C++ symbol prefix when multiple generated sidecars share one binary",
    )
    args = parser.parse_args()

    libapp = args.libapp.read_bytes()
    identity = _parse_elf64(libapp)
    artifact_function_name = args.artifact_function_name or args.function_name
    log, code_identity = _run_gen_snapshot(
        args.gen_snapshot,
        args.dill,
        args.library_uri,
        args.class_name,
        args.function_name,
        artifact_function_name,
    )
    machine = _extract_machine_code(log, args.function_name, args.compiler_function_kind)
    entry_points = _extract_entry_points(log, machine.printed_name)
    closure_call_entry_only = machine.function_kind in {
        "ClosureFunction",
        "ImplicitClosureFunction",
    }
    if closure_call_entry_only and args.entry_kind != "default":
        raise ValueError(
            f"{machine.function_kind} is invoked through cached Closure.entry_point in PRODUCT "
            "AOT; only the default entry can be emitted as closure-call evidence"
        )
    selected_compiler_entry = entry_points[_ENTRY_KIND_LABELS[args.entry_kind]]
    selected_offset = selected_compiler_entry - machine.start_address
    if selected_offset < 0 or selected_offset >= len(machine.code) or selected_offset % 4 != 0:
        raise ValueError(
            f"compiler {args.entry_kind} entry is outside the disassembled Code payload: "
            f"start=0x{machine.start_address:x} entry=0x{selected_compiler_entry:x}"
        )
    abi: AbiEvidence | None = None
    if not args.identity_only:
        parent_abi = _load_abi_oracle(
            args.abi_oracle_json,
            args.library_uri,
            args.class_name,
            args.function_name,
        )
        if closure_call_entry_only:
            closure_cfg = _extract_abi(
                log, args.function_name, exact_printed_name=machine.printed_name
            )
            abi = _derive_closure_abi_from_cfg(parent_abi, closure_cfg)
        else:
            abi = parent_abi
        # gen_snapshot's human-readable CFG is intentionally only an independent
        # cross-check. Runtime ABI evidence is emitted from vm.unboxing-info.metadata
        # above for ordinary Functions. Synthetic closures have no independent
        # Kernel-member metadata record, so their exact selected CFG is accepted
        # only through _derive_closure_abi_from_cfg(), which enforces the
        # separately source-verified boxed/forced-stack closure contract.
        if not closure_call_entry_only:
            cfg_abi = _extract_abi(log, args.function_name)
            _validate_cfg_cross_check(abi, cfg_abi)
    required_direct_call_target_va: int | None = None
    if closure_call_entry_only and args.compiler_function_kind == "ImplicitClosureFunction":
        ordinary_machine = _extract_machine_code(log, args.function_name, "RegularFunction")
        ordinary_entry_points = _extract_entry_points(log, ordinary_machine.printed_name)
        ordinary_payload_va, _, _ = _find_unique_executable_code(
            libapp, identity, ordinary_machine.code
        )
        ordinary_normal_offset = (
            ordinary_entry_points["kNormal"] - ordinary_machine.start_address
        )
        if ordinary_normal_offset < 0 or ordinary_normal_offset >= len(ordinary_machine.code):
            raise ValueError("related ordinary Function normal entry is outside its Code payload")
        required_direct_call_target_va = ordinary_payload_va + ordinary_normal_offset
    payload_va, actual_payload, location_mode = _find_unique_executable_code(
        libapp, identity, machine.code, required_direct_call_target_va
    )
    entry_va = payload_va + selected_offset
    actual_code = actual_payload[selected_offset:]
    if not actual_code:
        raise ValueError("selected Dart entry has no executable payload")
    structural: dict[str, object] | None = None
    if args.aot_analyzer is not None:
        structural = _run_machine_code_analyzer(args.aot_analyzer, actual_code, entry_va)
        if abi is not None:
            _validate_machine_code_cross_check(abi, structural)
    record: dict[str, object] = {
        "library_uri": args.library_uri,
        "class_name": args.class_name,
        "function_name": artifact_function_name,
        "source_function_name": args.function_name,
        "entry_kind": args.entry_kind,
        "entry_va": entry_va,
        "code_size": len(actual_code),
        "code_payload_va": payload_va,
        "code_instructions_length": len(actual_payload),
        "fingerprint": _fnv1a64(actual_code),
        "build_id": identity.build_id,
        "snapshot_hash": identity.snapshot_hash,
        "code_identity_proof": code_identity.proof,
        "physical_entry_alias_count": code_identity.physical_entry_alias_count,
        "function_kind": _FUNCTION_KIND_VALUES.get(machine.function_kind, 0),
        "function_kind_name": machine.function_kind,
        "closure_call_entry_only": closure_call_entry_only,
        "related_direct_call_target_va": required_direct_call_target_va,
    }
    if abi is not None:
        record.update(
            {
                "abi_parameters": list(abi.parameters),
                "abi_result": abi.result,
                "max_parameters_in_registers": abi.max_parameters_in_registers,
                "must_use_stack_calling_convention": abi.must_use_stack_calling_convention,
                "has_optional_parameters": abi.has_optional_parameters,
                "has_overrides_with_less_direct_parameters": (
                    abi.has_overrides_with_less_direct_parameters
                ),
            }
        )
    if structural is not None:
        record["structural_analysis"] = {
            "decoded_instructions": structural.get("decoded_instructions", 0),
            "basic_block_count": structural.get("basic_block_count", 0),
            "has_unknown_control_flow": structural.get("has_unknown_control_flow", False),
            "uses_arguments_descriptor": structural.get("uses_arguments_descriptor", False),
            "reached_return": structural.get("reached_return", False),
            "external_branch_count": len(structural.get("external_branches", [])),
            "indirect_call_count": len(structural.get("indirect_call_sites", [])),
            "indirect_branch_count": len(structural.get("indirect_branch_sites", [])),
            "direct_call_count": len(structural.get("direct_calls", [])),
            "return_site_count": len(structural.get("return_sites", [])),
            "address_materialization_count": len(
                structural.get("address_materializations", [])
            ),
        }
    if args.identity_only:
        _write_identity_header(args.output_header, record, args.symbol_prefix)
    else:
        _write_header(args.output_header, record, args.symbol_prefix)
    if args.output_json is not None:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(record, indent=2) + "\n")
    print(
        f"compiler sidecar: {args.library_uri}/{args.class_name}/{args.function_name} "
        f"entry={args.entry_kind} entry_va=0x{entry_va:x} size={len(actual_code)} "
        f"fingerprint={record['fingerprint']} "
        f"locator={location_mode} "
        f"identity={code_identity.proof}/{code_identity.physical_entry_alias_count} "
        f"abi={f'{abi.parameters}->{abi.result}' if abi is not None else 'identity-only'} "
        f"structural={'on' if structural is not None else 'off'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
