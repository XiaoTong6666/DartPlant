from __future__ import annotations

import argparse
import json
import re
import subprocess as sp
from pathlib import Path

from util import ROOT_DIR


MANIFEST = ROOT_DIR / "scripts" / "data" / "dart_vm_profiles.json"
GENERATED = ROOT_DIR / "src" / "vm" / "generated" / "runtime_profiles.generated.h"

EXPECTED_GP_ARGS = [1, 2, 3, 5, 6, 7]
EXPECTED_FPU_ARGS = [0, 1, 2, 3, 4, 5]
EXPECTED_FIXED_REGISTERS = {
    "thr": 26,
    "pp": 27,
    "code": 24,
    "heap_bits": 28,
    "null": 22,
    "spreg": 15,
    "args_desc": 4,
}

SDK_REGISTER_PATTERNS = {
    "thr": r"const Register THR = R26;",
    "pp": r"const Register PP = R27;",
    "code": r"const Register CODE_REG = R24;",
    "heap_bits": r"const Register HEAP_BITS = R28;",
    "null": r"const Register NULL_REG = R22;",
    "spreg": r"const Register SPREG = R15;",
    "args_desc": r"const Register ARGS_DESC_REG = R4;",
}

AOT_BLOCK_PATTERN = re.compile(
    r"#if defined\(PRODUCT\) && defined\(TARGET_ARCH_ARM64\) &&\s*\\\n"
    r"\s*defined\(DART_COMPRESSED_POINTERS\)"
)

AOT_OFFSET_BINDINGS = {
    ("instructions", "monomorphic_entry_offset_aot"): (
        "AOT_Instructions_kMonomorphicEntryOffsetAOT"
    ),
    ("instructions", "polymorphic_entry_offset_aot"): (
        "AOT_Instructions_kPolymorphicEntryOffsetAOT"
    ),
    ("thread", "heap_base"): "AOT_Thread_heap_base_offset",
    ("thread", "object_null"): "AOT_Thread_object_null_offset",
    ("thread", "global_object_pool"): "AOT_Thread_global_object_pool_offset",
    ("thread", "isolate"): "AOT_Thread_isolate_offset",
    ("thread", "isolate_group"): "AOT_Thread_isolate_group_offset",
    ("thread", "jump_to_frame_entry_point"): "AOT_Thread_jump_to_frame_entry_point_offset",
    ("isolate_group", "class_table"): "AOT_IsolateGroup_class_table_offset",
    ("isolate_group", "cached_class_table_table"): (
        "AOT_IsolateGroup_cached_class_table_table_offset"
    ),
    ("isolate_group", "object_store"): "AOT_IsolateGroup_object_store_offset",
    ("code", "entry_point"): "AOT_Code_entry_point_offset",
    ("code", "object_pool"): "AOT_Code_object_pool_offset",
    ("code", "owner"): "AOT_Code_owner_offset",
    ("function", "entry_point"): "AOT_Function_entry_point_offset",
    ("function", "code"): "AOT_Function_code_offset",
    ("function", "kind_tag"): "AOT_Function_kind_tag_offset",
    ("function", "signature"): "AOT_Function_signature_offset",
    ("array", "length"): "AOT_Array_length_offset",
    ("array", "elements"): "AOT_Array_data_offset",
    ("growable_object_array", "length"): "AOT_GrowableObjectArray_length_offset",
    ("growable_object_array", "data"): "AOT_GrowableObjectArray_data_offset",
    ("string", "length"): "AOT_String_length_offset",
    ("string", "data"): "AOT_OneByteString_data_offset",
    ("canonical_bool", "thread_true"): "AOT_Thread_bool_true_offset",
    ("canonical_bool", "thread_false"): "AOT_Thread_bool_false_offset",
    ("function_type", "abstract_type_flags"): "AOT_AbstractType_flags_offset",
    ("function_type", "type_parameters"): "AOT_FunctionType_type_parameters_offset",
    ("function_type", "parameter_types"): "AOT_FunctionType_parameter_types_offset",
    ("function_type", "named_parameter_names"): (
        "AOT_FunctionType_named_parameter_names_offset"
    ),
    ("function_type", "packed_parameter_counts"): (
        "AOT_FunctionType_packed_parameter_counts_offset"
    ),
    ("function_type", "packed_type_parameter_counts"): (
        "AOT_FunctionType_packed_type_parameter_counts_offset"
    ),
}

# CodeEntryKind order is defined by runtime/vm/code_entry_kind.h:
# normal, unchecked, monomorphic, monomorphic-unchecked. Function caches only
# normal and unchecked. Keep the non-default slots explicit in the manifest so
# live resolution never derives private raw-object offsets from adjacency.
AOT_ARRAY_OFFSET_BINDINGS = {
    ("code", "entry_point"): ("AOT_Code_entry_point_offset", 0),
    ("code", "unchecked_entry_point"): ("AOT_Code_entry_point_offset", 1),
    ("code", "monomorphic_entry_point"): ("AOT_Code_entry_point_offset", 2),
    ("code", "monomorphic_unchecked_entry_point"): ("AOT_Code_entry_point_offset", 3),
    ("function", "entry_point"): ("AOT_Function_entry_point_offset", 0),
    ("function", "unchecked_entry_point"): ("AOT_Function_entry_point_offset", 1),
}


def _u(value: int) -> str:
    return f"0x{value:x}u"


def _product_arm64_compressed_aot_block(text: str) -> str:
    matches = list(AOT_BLOCK_PATTERN.finditer(text))
    for match in matches:
        end = text.find("#endif", match.start())
        if end < 0:
            continue
        block = text[match.start() : end]
        if "AOT_Function_code_offset" in block:
            return block
    raise ValueError("PRODUCT ARM64 compressed AOT offset block was not found")


def _parse_aot_offset(block: str, name: str) -> int:
    match = re.search(
        rf"\b{re.escape(name)}\s*(?:\[\])?\s*=\s*([^;]+);",
        block,
        re.DOTALL,
    )
    if match is None:
        raise ValueError(f"Dart AOT offset is missing: {name}")
    expression = " ".join(match.group(1).split())
    if expression.startswith("{"):
        first = expression.removeprefix("{").split(",", 1)[0].strip()
        return int(first, 0)
    return int(expression, 0)


def _parse_aot_offset_at(block: str, name: str, index: int) -> int:
    match = re.search(
        rf"\b{re.escape(name)}\s*(?:\[\])?\s*=\s*([^;]+);",
        block,
        re.DOTALL,
    )
    if match is None:
        raise ValueError(f"Dart AOT offset is missing: {name}")
    expression = " ".join(match.group(1).split())
    if not expression.startswith("{"):
        if index != 0:
            raise ValueError(f"Dart AOT offset {name} is not an array")
        return int(expression, 0)
    values = [
        value.strip()
        for value in expression.removeprefix("{").removesuffix("}").split(",")
        if value.strip()
    ]
    if index >= len(values):
        raise ValueError(f"Dart AOT offset {name}[{index}] is missing")
    return int(values[index], 0)


def _verify_profile_against_aot_offsets(
    profile: dict[str, object], runtime_offsets: str
) -> None:
    block = _product_arm64_compressed_aot_block(runtime_offsets)
    name = str(profile["name"])
    for (section, field), sdk_name in AOT_OFFSET_BINDINGS.items():
        if (section, field) in AOT_ARRAY_OFFSET_BINDINGS:
            continue
        expected = int(profile[section][field])
        actual = _parse_aot_offset(block, sdk_name)
        if actual != expected:
            raise ValueError(
                f"{name}: manifest {section}.{field}=0x{expected:x} disagrees "
                f"with {sdk_name}=0x{actual:x}"
            )
    for (section, field), (sdk_name, index) in AOT_ARRAY_OFFSET_BINDINGS.items():
        expected = int(profile[section][field])
        actual = _parse_aot_offset_at(block, sdk_name, index)
        if actual != expected:
            raise ValueError(
                f"{name}: manifest {section}.{field}=0x{expected:x} disagrees "
                f"with {sdk_name}[{index}]=0x{actual:x}"
            )



def _verify_aot_payload_contract(
    object_header: str, app_snapshot: str, *, source_name: str
) -> None:
    object_text = " ".join(object_header.split())
    snapshot_text = " ".join(app_snapshot.split())
    for evidence in (
        "return code->untag()->entry_point_ != code->untag()->monomorphic_entry_point_;",
        "const uword entry_offset = HasMonomorphicEntry(code) ? Instructions::kPolymorphicEntryOffsetAOT : 0;",
        "return EntryPointOf(code) - entry_offset;",
    ):
        if evidence not in object_text:
            raise ValueError(
                f"{source_name}: Dart precompiled Code::PayloadStartOf contract changed: {evidence}"
            )
    for evidence in (
        "uword start = Code::PayloadStartOf(code);",
        "code->untag()->instructions_length_ = previous_end - start;",
    ):
        if evidence not in snapshot_text:
            raise ValueError(
                f"{source_name}: Dart instructions_length_ payload contract changed: {evidence}"
            )


def _git_show(sdk_root: Path, revision: str, path: str) -> str:
    result = sp.run(
        ["git", "-C", str(sdk_root), "show", f"{revision}:{path}"],
        check=False,
        stdout=sp.PIPE,
        stderr=sp.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise ValueError(
            f"Dart SDK revision {revision} cannot provide {path}: {result.stderr.strip()}"
        )
    return result.stdout


def verify_historical_profiles(sdk_root: Path, profiles: list[dict[str, object]]) -> None:
    git_dir = sdk_root / ".git"
    if not git_dir.exists():
        raise ValueError(f"Dart SDK source checkout is not a git repository: {sdk_root}")
    for profile in profiles:
        version = str(profile["dart_version"])
        runtime_offsets = _git_show(
            sdk_root, version, "runtime/vm/compiler/runtime_offsets_extracted.h"
        )
        object_header = _git_show(sdk_root, version, "runtime/vm/object.h")
        app_snapshot = _git_show(sdk_root, version, "runtime/vm/app_snapshot.cc")
        _verify_profile_against_aot_offsets(profile, runtime_offsets)
        _verify_aot_payload_contract(
            object_header, app_snapshot, source_name=f"Dart SDK {version}"
        )
        print(f"Dart SDK AOT profile verified: {profile['name']} @ {version}")


def _load_manifest(path: Path = MANIFEST) -> list[dict[str, object]]:
    payload = json.loads(path.read_text())
    if payload.get("schema_version") != 1:
        raise ValueError("dart VM profile manifest schema_version must be 1")
    profiles = payload.get("profiles")
    if not isinstance(profiles, list) or not profiles:
        raise ValueError("dart VM profile manifest must contain profiles")

    versions: set[int] = set()
    hashes: set[str] = set()
    names: set[str] = set()
    for profile in profiles:
        version = int(profile["profile_version"])
        name = str(profile["name"])
        snapshot_hash = str(profile["snapshot_hash"])
        if version in versions or name in names or snapshot_hash in hashes:
            raise ValueError("dart VM profile identities must be unique")
        versions.add(version)
        names.add(name)
        hashes.add(snapshot_hash)
        if len(snapshot_hash) != 32 or any(c not in "0123456789abcdef" for c in snapshot_hash):
            raise ValueError(f"invalid snapshot hash for {name}: {snapshot_hash}")

        registers = profile["registers"]
        for key, expected in EXPECTED_FIXED_REGISTERS.items():
            if int(registers[key]) != expected:
                raise ValueError(
                    f"{name}: {key} register disagrees with Dart ARM64 constants: "
                    f"{registers[key]} != {expected}"
                )
        if list(registers["dart_gp_args"]) != EXPECTED_GP_ARGS:
            raise ValueError(f"{name}: Dart GP calling convention changed")
        if list(registers["dart_fpu_args"]) != EXPECTED_FPU_ARGS:
            raise ValueError(f"{name}: Dart FPU calling convention changed")

        thread = profile["thread"]
        if int(thread["jump_to_frame_entry_point"]) == 0:
            raise ValueError(f"{name}: JumpToFrame Thread offset must be non-zero")
        if int(profile["function"]["signature"]) == 0:
            raise ValueError(f"{name}: Function.signature offset must be non-zero")
        instructions = profile["instructions"]
        if int(instructions["monomorphic_entry_offset_aot"]) <= 0 or int(
            instructions["polymorphic_entry_offset_aot"]
        ) <= int(instructions["monomorphic_entry_offset_aot"]):
            raise ValueError(f"{name}: invalid ARM64 AOT instruction entry offsets")
    return profiles


def verify_sdk_contract(sdk_root: Path) -> None:
    constants = sdk_root / "runtime" / "vm" / "constants_arm64.h"
    calling = (
        sdk_root
        / "runtime"
        / "vm"
        / "compiler"
        / "backend"
        / "dart_calling_conventions.cc"
    )
    thread = sdk_root / "runtime" / "vm" / "thread.h"
    function_impl = sdk_root / "runtime" / "vm" / "object.cc"
    code_entry_kind = sdk_root / "runtime" / "vm" / "code_entry_kind.h"
    raw_object = sdk_root / "runtime" / "vm" / "raw_object.h"
    object_header = sdk_root / "runtime" / "vm" / "object.h"
    il_arm64 = sdk_root / "runtime" / "vm" / "compiler" / "backend" / "il_arm64.cc"
    for path in (
        constants,
        calling,
        thread,
        function_impl,
        code_entry_kind,
        raw_object,
        object_header,
        il_arm64,
    ):
        if not path.is_file():
            raise ValueError(f"Dart SDK source contract file is missing: {path}")

    constants_text = constants.read_text()
    for name, pattern in SDK_REGISTER_PATTERNS.items():
        if re.search(pattern, constants_text) is None:
            raise ValueError(f"Dart SDK ARM64 register contract changed: {name}")
    gp = ", ".join(f"R{value}" for value in EXPECTED_GP_ARGS)
    fpu = ", ".join(f"V{value}" for value in EXPECTED_FPU_ARGS)
    if f"kCpuRegistersForArgs[] = {{{gp}}}" not in constants_text:
        raise ValueError("Dart SDK ARM64 GP argument register sequence changed")
    normalized = " ".join(constants_text.split())
    if f"kFpuRegistersForArgs[] = {{{fpu}}};" not in normalized:
        raise ValueError("Dart SDK ARM64 FPU argument register sequence changed")

    calling_text = calling.read_text()
    required_allocator_lines = (
        "SimpleAllocator cpu_allocator(DartCallingConvention::kCpuRegistersForArgs);",
        "SimpleAllocator fpu_allocator(DartCallingConvention::kFpuRegistersForArgs);",
    )
    if any(line not in calling_text for line in required_allocator_lines):
        raise ValueError("Dart SDK no longer uses independent GP/FPU Dart argument allocators")
    if "target.MaxNumberOfParametersInRegisters(zone)" not in calling_text:
        raise ValueError("Dart SDK register-parameter limit contract changed")

    function_text = function_impl.read_text()
    register_cc_start = function_text.find("Function::MaxNumberOfParametersInRegisters")
    if register_cc_start < 0:
        raise ValueError("Dart Function register-CC policy implementation was not found")
    register_cc = function_text[register_cc_start : register_cc_start + 5000]
    for kind in (
        "kClosureFunction",
        "kImplicitClosureFunction",
        "kNoSuchMethodDispatcher",
        "kInvokeFieldDispatcher",
        "kDynamicInvocationForwarder",
    ):
        if kind not in register_cc:
            raise ValueError(f"Dart Function register-CC policy no longer names {kind}")
    if "must_use_stack_calling_convention" not in register_cc:
        raise ValueError("Dart Function no longer consumes unboxing forced-stack metadata")

    entry_kind_text = code_entry_kind.read_text()
    entry_positions = [
        entry_kind_text.find(name)
        for name in ("kNormal", "kUnchecked", "kMonomorphic", "kMonomorphicUnchecked")
    ]
    if any(position < 0 for position in entry_positions) or entry_positions != sorted(entry_positions):
        raise ValueError("Dart CodeEntryKind order changed")

    raw_object_text = raw_object.read_text()
    for field in (
        "uword entry_point_",
        "uword unchecked_entry_point_",
        "uword monomorphic_entry_point_",
        "uword monomorphic_unchecked_entry_point_",
    ):
        if field not in raw_object_text:
            raise ValueError(f"Dart raw entry-point cache changed: {field}")

    object_header_text = object_header.read_text()
    _verify_aot_payload_contract(
        object_header_text,
        (sdk_root / "runtime" / "vm" / "app_snapshot.cc").read_text(),
        source_name="current Dart SDK",
    )
    if "untag()->entry_point_ = function.entry_point();" not in object_header_text:
        raise ValueError("Dart Closure no longer caches Function.entry_point on set_function")
    il_arm64_text = il_arm64.read_text()
    closure_call_start = il_arm64_text.find("void ClosureCallInstr::EmitNativeCode")
    if closure_call_start < 0:
        raise ValueError("Dart ARM64 ClosureCall lowering was not found")
    closure_call = il_arm64_text[closure_call_start : closure_call_start + 2600]
    for evidence in (
        "R0: Closure with a cached entry point.",
        "compiler::target::Closure::entry_point_offset()",
        "__ blr(R2);",
    ):
        if evidence not in closure_call:
            raise ValueError(f"Dart ARM64 closure-call ABI changed: {evidence}")

    thread_text = thread.read_text()
    if "jump_to_frame_entry_point_" not in thread_text or "StubCode::JumpToFrame().EntryPoint()" not in thread_text:
        raise ValueError("Dart Thread no longer exposes the JumpToFrame cached entry point")
    print(f"Dart SDK ARM64 source contract verified: {sdk_root}")


def _render_profile(profile: dict[str, object]) -> str:
    r = profile["registers"]
    thread = profile["thread"]
    isolate_group = profile["isolate_group"]
    class_table = profile["class_table"]
    object_store = profile["object_store"]
    instructions = profile["instructions"]
    code = profile["code"]
    function = profile["function"]
    klass = profile["class"]
    library = profile["library"]
    array = profile["array"]
    growable = profile["growable_object_array"]
    string = profile["string"]
    object_pool = profile["object_pool"]
    cids = profile["cids"]
    boolean = profile["canonical_bool"]
    function_type = profile["function_type"]
    gp_args = ", ".join(str(value) for value in r["dart_gp_args"])
    fpu_args = ", ".join(str(value) for value in r["dart_fpu_args"])
    return f"""    RuntimeProfileRecord{{
        .live_vm = {{
            .struct_size = sizeof(DartPlantLiveVmProfile),
            .profile_version = {profile['profile_version']}u,
            .name = {json.dumps(profile['name'])},
            .dart_version = {json.dumps(profile['dart_version'])},
            .snapshot_hash = {json.dumps(profile['snapshot_hash'])},
            .snapshot_profile = {json.dumps(profile['snapshot_profile'])},
            .thr_register = {r['thr']}u,
            .pp_register = {r['pp']}u,
            .code_register = {r['code']}u,
            .heap_bits_register = {r['heap_bits']}u,
            .null_register = {r['null']}u,
            .reserved_registers = {{0u, 0u, 0u}},
            .thread_heap_base_offset = {_u(int(thread['heap_base']))},
            .thread_object_null_offset = {_u(int(thread['object_null']))},
            .thread_global_object_pool_offset = {_u(int(thread['global_object_pool']))},
            .thread_isolate_offset = {_u(int(thread['isolate']))},
            .thread_isolate_group_offset = {_u(int(thread['isolate_group']))},
            .isolate_group_class_table_offset = {_u(int(isolate_group['class_table']))},
            .isolate_group_cached_class_table_table_offset = {_u(int(isolate_group['cached_class_table_table']))},
            .isolate_group_object_store_offset = {_u(int(isolate_group['object_store']))},
            .class_table_num_cids_offset = {_u(int(class_table['num_cids']))},
            .object_store_libraries_offset = {_u(int(object_store['libraries']))},
            .code_entry_point_offset = {_u(int(code['entry_point']))},
            .code_object_pool_offset = {_u(int(code['object_pool']))},
            .code_owner_offset = {_u(int(code['owner']))},
            .code_instructions_length_offset = {_u(int(code['instructions_length']))},
            .function_entry_point_offset = {_u(int(function['entry_point']))},
            .function_name_offset = {_u(int(function['name']))},
            .function_owner_offset = {_u(int(function['owner']))},
            .function_code_offset = {_u(int(function['code']))},
            .function_kind_tag_offset = {_u(int(function['kind_tag']))},
            .class_name_offset = {_u(int(klass['name']))},
            .class_functions_offset = {_u(int(klass['functions']))},
            .class_library_offset = {_u(int(klass['library']))},
            .library_url_offset = {_u(int(library['url']))},
            .library_toplevel_class_offset = {_u(int(library['toplevel_class']))},
            .array_length_offset = {_u(int(array['length']))},
            .array_elements_offset = {_u(int(array['elements']))},
            .growable_object_array_length_offset = {_u(int(growable['length']))},
            .growable_object_array_data_offset = {_u(int(growable['data']))},
            .string_length_offset = {_u(int(string['length']))},
            .string_data_offset = {_u(int(string['data']))},
            .object_pool_length_offset = {_u(int(object_pool['length']))},
            .object_pool_elements_offset = {_u(int(object_pool['elements']))},
            .cid_class = {cids['class']}u,
            .cid_function = {cids['function']}u,
            .cid_library = {cids['library']}u,
            .cid_code = {cids['code']}u,
            .cid_object_pool = {cids['object_pool']}u,
            .cid_array = {cids['array']}u,
            .cid_immutable_array = {cids['immutable_array']}u,
            .cid_growable_object_array = {cids['growable_object_array']}u,
            .cid_one_byte_string = {cids['one_byte_string']}u,
            .cid_two_byte_string = {cids['two_byte_string']}u,
            .code_unchecked_entry_point_offset = {_u(int(code['unchecked_entry_point']))},
            .code_monomorphic_entry_point_offset = {_u(int(code['monomorphic_entry_point']))},
            .code_monomorphic_unchecked_entry_point_offset = {_u(int(code['monomorphic_unchecked_entry_point']))},
            .function_unchecked_entry_point_offset = {_u(int(function['unchecked_entry_point']))},
        }},
        .dart_sp_register = {r['spreg']}u,
        .arguments_descriptor_register = {r['args_desc']}u,
        .dart_gp_argument_registers = {{{gp_args}}},
        .dart_fpu_argument_registers = {{{fpu_args}}},
        .instructions_monomorphic_entry_offset_aot = {_u(int(instructions['monomorphic_entry_offset_aot']))},
        .instructions_polymorphic_entry_offset_aot = {_u(int(instructions['polymorphic_entry_offset_aot']))},
        .thread_jump_to_frame_entry_point_offset = {_u(int(thread['jump_to_frame_entry_point']))},
        .canonical_bool = {{
            .thread_true_offset = {_u(int(boolean['thread_true']))},
            .thread_false_offset = {_u(int(boolean['thread_false']))},
            .value_offset = {_u(int(boolean['value']))},
            .cid = {boolean['cid']}u,
        }},
        .function_type = {{
            .function_signature_offset = {_u(int(function['signature']))},
            .abstract_type_flags_offset = {_u(int(function_type['abstract_type_flags']))},
            .type_parameters_offset = {_u(int(function_type['type_parameters']))},
            .result_type_offset = {_u(int(function_type['result_type']))},
            .parameter_types_offset = {_u(int(function_type['parameter_types']))},
            .named_parameter_names_offset = {_u(int(function_type['named_parameter_names']))},
            .packed_parameter_counts_offset = {_u(int(function_type['packed_parameter_counts']))},
            .packed_type_parameter_counts_offset = {_u(int(function_type['packed_type_parameter_counts']))},
            .cid_type = {function_type['cid_type']}u,
            .cid_function_type = {function_type['cid_function_type']}u,
            .cid_record_type = {function_type['cid_record_type']}u,
            .cid_type_parameter = {function_type['cid_type_parameter']}u,
            .cid_null = {function_type['cid_null']}u,
            .cid_dynamic = {function_type['cid_dynamic']}u,
            .cid_void = {function_type['cid_void']}u,
            .cid_never = {function_type['cid_never']}u,
            .type_parameter_base_offset = {_u(int(function_type['type_parameter_base']))},
            .type_parameter_index_offset = {_u(int(function_type['type_parameter_index']))},
            .nullability_bits = {function_type['nullability_bits']}u,
            .type_class_id_shift = {function_type['type_class_id_shift']}u,
            .type_parameter_function_bit = {function_type['type_parameter_function_bit']}u,
        }},
    }}"""


def render(path: Path = MANIFEST) -> str:
    profiles = _load_manifest(path)
    body = ",\n".join(_render_profile(profile) for profile in profiles)
    return f"""// Generated by scripts/generate_vm_profiles.py. Do not edit.
#pragma once

namespace dartplant {{

inline constexpr RuntimeProfileRecord kGeneratedRuntimeProfiles[] = {{
{body},
}};

}}  // namespace dartplant
"""


def run(*, check: bool, sdk_root: Path | None = None) -> None:
    profiles = _load_manifest()
    expected = render()
    if sdk_root is not None:
        verify_sdk_contract(sdk_root)
        verify_historical_profiles(sdk_root, profiles)
    if check:
        if not GENERATED.is_file() or GENERATED.read_text() != expected:
            raise RuntimeError(
                "generated Dart VM profiles are stale; run "
                "python3 scripts/main.py profiles"
            )
        print(f"VM profiles verified: {GENERATED.relative_to(ROOT_DIR)}")
        return
    GENERATED.parent.mkdir(parents=True, exist_ok=True)
    GENERATED.write_text(expected)
    print(f"VM profiles generated: {GENERATED.relative_to(ROOT_DIR)}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--sdk-root", type=Path)
    args = parser.parse_args()
    run(check=args.check, sdk_root=args.sdk_root)


if __name__ == "__main__":
    main()
