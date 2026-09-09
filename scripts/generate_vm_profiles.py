from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import subprocess as sp
from pathlib import Path

from util import ROOT_DIR


MANIFEST = ROOT_DIR / "scripts" / "data" / "dart_vm_profiles.json"
CAPABILITY_MANIFEST = ROOT_DIR / "scripts" / "data" / "vm_capabilities.json"
GENERATED = ROOT_DIR / "src" / "vm" / "generated" / "runtime_profiles.generated.h"
GENERATED_FINGERPRINTS = (
    ROOT_DIR / "src" / "vm" / "generated" / "capability_fingerprints.generated.h"
)
GENERATED_CAPABILITIES = (
    ROOT_DIR / "src" / "vm" / "generated" / "capability_registry.generated.h"
)

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

ARTIFACT_ONLY_ABI_FIELD_NAMES = {
    "entry_va",
    "runtime_entry",
    "code_payload_va",
    "code_size",
    "code_fingerprint",
    "fingerprint",
    "build_id",
    "load_bias",
    "snapshot_symbol_va",
    "snapshot_symbol_file_offset",
    "snapshot_symbol_runtime",
    "snapshot_symbol_size",
    "artifact_generation",
    "isolate_generation",
}

PROVENANCE_ONLY_PROFILE_FIELDS = {
    "snapshot_hash",
    "dart_version",
    "name",
    "snapshot_profile",
    "profile_version",
    "abi_identity",
    "abi_id",
}

EXPECTED_SNAPSHOT_SYMBOLS = {
    "kSnapshotBuildIdAsmSymbol": "_kDartSnapshotBuildId",
    "kVmSnapshotDataAsmSymbol": "_kDartVmSnapshotData",
    "kVmSnapshotInstructionsAsmSymbol": "_kDartVmSnapshotInstructions",
    "kVmSnapshotBssAsmSymbol": "_kDartVmSnapshotBss",
    "kIsolateSnapshotDataAsmSymbol": "_kDartIsolateSnapshotData",
    "kIsolateSnapshotInstructionsAsmSymbol": "_kDartIsolateSnapshotInstructions",
    "kIsolateSnapshotBssAsmSymbol": "_kDartIsolateSnapshotBss",
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

NONPRODUCT_AOT_BLOCK_PATTERN = re.compile(
    r"#if !defined\(PRODUCT\) && defined\(TARGET_ARCH_ARM64\) &&\s*\\\n"
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
    ("thread", "enter_safepoint_stub"): "AOT_Thread_enter_safepoint_stub_offset",
    ("thread", "exit_safepoint_stub"): "AOT_Thread_exit_safepoint_stub_offset",
    ("thread", "top_exit_frame"): "AOT_Thread_top_exit_frame_info_offset",
    ("thread", "vm_tag"): "AOT_Thread_vm_tag_offset",
    ("thread", "active_exception"): "AOT_Thread_active_exception_offset",
    ("thread", "active_stacktrace"): "AOT_Thread_active_stacktrace_offset",
    ("thread", "execution_state"): "AOT_Thread_execution_state_offset",
    ("thread", "exit_through_ffi"): "AOT_Thread_exit_through_ffi_offset",
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
    ("arguments_descriptor", "type_args_len"): "AOT_ArgumentsDescriptor_type_args_len_offset",
    ("arguments_descriptor", "count"): "AOT_ArgumentsDescriptor_count_offset",
    ("arguments_descriptor", "size"): "AOT_ArgumentsDescriptor_size_offset",
    ("arguments_descriptor", "positional_count"): (
        "AOT_ArgumentsDescriptor_positional_count_offset"
    ),
    ("arguments_descriptor", "first_named_entry"): (
        "AOT_ArgumentsDescriptor_first_named_entry_offset"
    ),
    ("arguments_descriptor", "named_entry_size"): "AOT_ArgumentsDescriptor_named_entry_size",
    ("arguments_descriptor", "name"): "AOT_ArgumentsDescriptor_name_offset",
    ("arguments_descriptor", "position"): "AOT_ArgumentsDescriptor_position_offset",
    ("type_arguments", "length"): "AOT_TypeArguments_length_offset",
    ("type_arguments", "types"): "AOT_TypeArguments_types_offset",
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
    return _arm64_compressed_aot_block(text, product=True)


def _nonproduct_arm64_compressed_aot_block(text: str) -> str:
    return _arm64_compressed_aot_block(text, product=False)


def _arm64_compressed_aot_block(text: str, *, product: bool) -> str:
    pattern = AOT_BLOCK_PATTERN if product else NONPRODUCT_AOT_BLOCK_PATTERN
    matches = list(pattern.finditer(text))
    for match in matches:
        end = text.find("#endif", match.start())
        if end < 0:
            continue
        block = text[match.start() : end]
        if "AOT_Function_code_offset" in block:
            return block
    mode = "PRODUCT" if product else "!PRODUCT"
    raise ValueError(f"{mode} ARM64 compressed AOT offset block was not found")


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
    block = _arm64_compressed_aot_block(
        runtime_offsets, product=bool(profile["machine"]["product"])
    )
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


def _verify_class_table_num_cids_contract(
    profile: dict[str, object],
    runtime_offsets: str,
    class_table_header: str,
    *,
    source_name: str,
) -> None:
    """Verify the private ClassTable::NumCids() backing-field offset.

    runtime_offsets_extracted.h exposes ClassTable's !PRODUCT allocation-
    tracing pointer but not the nested CidIndexedTable::num_cids_ member.
    PRODUCT and !PRODUCT therefore need an explicit source-proven derivation
    instead of sharing the same manifest value.
    """

    pointer_size = int(profile["machine"]["pointer_size"])
    if pointer_size != 8:
        raise ValueError(
            f"{source_name}: unsupported ClassTable pointer size: {pointer_size}"
        )

    normalized = " ".join(class_table_header.split())
    for evidence in (
        "ClassTableAllocator* allocator_; intptr_t num_cids_ = 0; intptr_t capacity_ = 0;",
        "static_assert(sizeof(cached_allocation_tracing_state_table_) == kWordSize); return OFFSET_OF(ClassTable, cached_allocation_tracing_state_table_);",
        "NOT_IN_PRODUCT(AcqRelAtomic<uint8_t*> cached_allocation_tracing_state_table_ = {nullptr});",
    ):
        if evidence not in normalized:
            raise ValueError(
                f"{source_name}: ClassTable private layout contract changed: {evidence}"
            )

    allocator_positions = [
        match.start()
        for match in re.finditer(r"ClassTableAllocator\* allocator_;", normalized)
    ]
    tracing_position = normalized.find(
        "NOT_IN_PRODUCT(AcqRelAtomic<uint8_t*> cached_allocation_tracing_state_table_"
    )
    if len(allocator_positions) < 2 or tracing_position <= allocator_positions[-1]:
        raise ValueError(
            f"{source_name}: ClassTable allocator/tracing-field order changed"
        )

    product = bool(profile["machine"]["product"])
    if product:
        # ClassTable::allocator_ followed by CidIndexedTable::allocator_, then
        # CidIndexedTable::num_cids_.
        expected = pointer_size * 2
    else:
        block = _arm64_compressed_aot_block(runtime_offsets, product=False)
        tracing_offset = _parse_aot_offset(
            block, "AOT_ClassTable_allocation_tracing_state_table_offset"
        )
        if tracing_offset != pointer_size:
            raise ValueError(
                f"{source_name}: !PRODUCT ClassTable allocation-tracing field moved: "
                f"0x{tracing_offset:x} != 0x{pointer_size:x}"
            )
        # The tracing pointer occupies one machine word at tracing_offset;
        # classes_ follows it, and nested CidIndexedTable starts with an
        # allocator pointer before num_cids_.
        expected = tracing_offset + pointer_size * 2

    actual = int(profile["class_table"]["num_cids"])
    if actual != expected:
        raise ValueError(
            f"{profile['name']}: manifest class_table.num_cids=0x{actual:x} "
            f"disagrees with source-proven {source_name} layout=0x{expected:x}"
        )



def _verify_class_raw_layout_contract(
    profile: dict[str, object], raw_object: str, *, source_name: str
) -> None:
    """Verify the precompiled UntaggedClass fields consumed by live indexing."""

    compressed_word_size = int(profile["raw_object"]["compressed_word_size"])
    if compressed_word_size != 4:
        raise ValueError(
            f"{source_name}: unsupported compressed UntaggedClass word size: "
            f"{compressed_word_size}"
        )

    normalized = " ".join(raw_object.split())
    ordered_fields = (
        "COMPRESSED_POINTER_FIELD(StringPtr, name)",
        "NOT_IN_PRODUCT(COMPRESSED_POINTER_FIELD(StringPtr, user_name))",
        "COMPRESSED_POINTER_FIELD(ArrayPtr, functions)",
        "COMPRESSED_POINTER_FIELD(ArrayPtr, functions_hash_table)",
        "COMPRESSED_POINTER_FIELD(ArrayPtr, fields)",
        "COMPRESSED_POINTER_FIELD(ArrayPtr, offset_in_words_to_field)",
        "COMPRESSED_POINTER_FIELD(ArrayPtr, interfaces)",
        "COMPRESSED_POINTER_FIELD(ScriptPtr, script)",
        "COMPRESSED_POINTER_FIELD(LibraryPtr, library)",
    )
    cursor = normalized.find("class UntaggedClass : public UntaggedObject")
    if cursor < 0:
        raise ValueError(f"{source_name}: UntaggedClass declaration is missing")
    for field in ordered_fields:
        position = normalized.find(field, cursor)
        if position < 0:
            raise ValueError(
                f"{source_name}: UntaggedClass live-index field order changed at {field}"
            )
        cursor = position + len(field)

    expected_name = compressed_word_size * 2
    expected_functions = expected_name + compressed_word_size
    if not bool(profile["machine"]["product"]):
        expected_functions += compressed_word_size
    expected_library = expected_functions + compressed_word_size * 6

    expected = {
        "name": expected_name,
        "functions": expected_functions,
        "library": expected_library,
    }
    actual = profile["class"]
    for field, value in expected.items():
        manifest_value = int(actual[field])
        if manifest_value != value:
            raise ValueError(
                f"{profile['name']}: manifest class.{field}=0x{manifest_value:x} "
                f"disagrees with source-proven {source_name} layout=0x{value:x}"
            )


def _verify_code_instructions_length_contract(
    profile: dict[str, object], raw_object: str, *, source_name: str
) -> None:
    """Verify UntaggedCode::instructions_length_ for PRODUCT and !PRODUCT AOT.

    The generated runtime-offset table exposes the early Code entry/object
    fields but not the trailing instructions_length_ field. In a precompiled
    !PRODUCT runtime, three diagnostic object pointers plus an aligned 64-bit
    compile timestamp are retained before state_bits_, shifting
    instructions_length_ by 0x20 relative to PRODUCT.
    """

    pointer_size = int(profile["machine"]["pointer_size"])
    if pointer_size != 8:
        raise ValueError(
            f"{source_name}: unsupported UntaggedCode pointer size: {pointer_size}"
        )

    normalized = " ".join(raw_object.split())
    start = normalized.find("class UntaggedCode : public UntaggedObject")
    if start < 0:
        raise ValueError(f"{source_name}: UntaggedCode declaration is missing")
    end = normalized.find("class UntaggedObjectPool", start)
    if end < 0:
        raise ValueError(f"{source_name}: UntaggedCode declaration is unterminated")
    code = normalized[start:end]

    ordered_fields = (
        "POINTER_FIELD(ObjectPtr, owner)",
        "POINTER_FIELD(ExceptionHandlersPtr, exception_handlers)",
        "POINTER_FIELD(PcDescriptorsPtr, pc_descriptors)",
        "POINTER_FIELD(ObjectPtr, catch_entry)",
        "POINTER_FIELD(CompressedStackMapsPtr, compressed_stackmaps)",
        "POINTER_FIELD(ArrayPtr, inlined_id_to_function)",
        "POINTER_FIELD(CodeSourceMapPtr, code_source_map)",
        "NOT_IN_PRODUCT(POINTER_FIELD(ObjectPtr, return_address_metadata))",
        "NOT_IN_PRODUCT(POINTER_FIELD(LocalVarDescriptorsPtr, var_descriptors))",
        "NOT_IN_PRODUCT(POINTER_FIELD(ArrayPtr, comments))",
        "NOT_IN_PRODUCT(alignas(8) int64_t compile_timestamp_);",
        "int32_t state_bits_;",
        "ONLY_IN_PRECOMPILED(uint32_t instructions_length_);",
    )
    cursor = 0
    for field in ordered_fields:
        position = code.find(field, cursor)
        if position < 0:
            raise ValueError(
                f"{source_name}: UntaggedCode AOT layout changed at {field}"
            )
        cursor = position + len(field)

    owner_offset = int(profile["code"]["owner"])
    # owner_ is followed by six always-present pointer fields through
    # code_source_map_. PRODUCT then stores state_bits_ and the 32-bit AOT
    # instructions length. !PRODUCT retains three more pointers and an aligned
    # 64-bit timestamp before those two scalar fields.
    expected = owner_offset + pointer_size * 7 + 4
    if not bool(profile["machine"]["product"]):
        expected += pointer_size * 4

    actual = int(profile["code"]["instructions_length"])
    if actual != expected:
        raise ValueError(
            f"{profile['name']}: manifest code.instructions_length=0x{actual:x} "
            f"disagrees with source-proven {source_name} layout=0x{expected:x}"
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


def _extract_v_macro(text: str, name: str) -> list[str]:
    lines = text.splitlines()
    start = next(
        (index for index, line in enumerate(lines) if line.startswith(f"#define {name}(V)")),
        None,
    )
    if start is None:
        raise ValueError(f"Dart source macro is missing: {name}")
    body: list[str] = []
    index = start
    while index < len(lines):
        line = lines[index]
        if index == start:
            line = line.split(")", 1)[1]
        continuation = line.rstrip().endswith("\\")
        body.append(line.rstrip().removesuffix("\\"))
        if not continuation:
            break
        index += 1
    joined = " ".join(body)
    values: list[str] = []
    token = re.compile(r"V\(([A-Za-z0-9_]+)\)|([A-Z][A-Z0-9_]+)\(V\)")
    for match in token.finditer(joined):
        direct, nested = match.groups()
        if direct is not None:
            values.append(direct)
        elif nested is not None:
            values.extend(_extract_v_macro(text, nested))
    return values


def _class_id_map(class_id_text: str) -> dict[str, int]:
    class_ids = ["IllegalCid", "NativePointer", "FreeListElement", "ForwardingCorpse"]
    class_ids.extend(f"{name}Cid" for name in _extract_v_macro(class_id_text, "CLASS_LIST"))
    if "CID(LinkedHashBaseCid)" in class_id_text or "kLinkedHashBaseCid" in class_id_text:
        class_ids.append("LinkedHashBaseCid")
    class_ids.extend(f"Ffi{name}Cid" for name in _extract_v_macro(class_id_text, "CLASS_LIST_FFI"))
    for name in _extract_v_macro(class_id_text, "CLASS_LIST_TYPED_DATA"):
        class_ids.extend(
            (
                f"TypedData{name}Cid",
                f"TypedData{name}ViewCid",
                f"ExternalTypedData{name}Cid",
                f"UnmodifiableTypedData{name}ViewCid",
            )
        )
    class_ids.extend(
        (
            "ByteDataViewCid",
            "UnmodifiableByteDataViewCid",
            "ByteBufferCid",
            "NullCid",
            "DynamicCid",
            "VoidCid",
            "NeverCid",
        )
    )
    return {name: index for index, name in enumerate(class_ids)}


ABI_PROFILE_SECTIONS = (
    "machine",
    "registers",
    "thread",
    "isolate_group",
    "class_table",
    "object_store",
    "instructions",
    "code",
    "function",
    "class",
    "library",
    "array",
    "growable_object_array",
    "string",
    "object_pool",
    "cids",
    "canonical_bool",
    "function_type",
    "raw_object",
    "arguments_descriptor",
    "function_kind",
    "type_arguments",
    "transition",
)

ABI_DOMAIN_FIELDS = {
    "core": (
        "machine.architecture", "machine.pointer_size", "machine.product",
        "machine.compressed_pointers", "thread.heap_base", "thread.object_null",
        "thread.global_object_pool", "thread.isolate", "thread.isolate_group",
        "isolate_group.class_table", "isolate_group.cached_class_table_table",
        "isolate_group.object_store", "class_table.num_cids", "object_store.libraries",
        "code.object_pool", "code.owner", "code.instructions_length", "function.name",
        "function.owner", "function.code", "class.name", "class.functions",
        "class.library", "library.url", "library.toplevel_class",
    ),
    "call": (
        "registers.thr", "registers.pp", "registers.code", "registers.heap_bits",
        "registers.null", "registers.spreg", "registers.args_desc",
        "registers.dart_gp_args", "registers.dart_fpu_args",
        "instructions.monomorphic_entry_offset_aot",
        "instructions.polymorphic_entry_offset_aot", "code.entry_point",
        "code.unchecked_entry_point", "code.monomorphic_entry_point",
        "code.monomorphic_unchecked_entry_point", "function.entry_point",
        "function.unchecked_entry_point", "function.kind_tag",
        "arguments_descriptor.type_args_len", "arguments_descriptor.count",
        "arguments_descriptor.size", "arguments_descriptor.positional_count",
        "arguments_descriptor.first_named_entry", "arguments_descriptor.named_entry_size",
        "arguments_descriptor.name", "arguments_descriptor.position",
        "function_kind.regular", "function_kind.closure", "function_kind.implicit_closure",
        "function_kind.tag_shift", "function_kind.tag_bits",
    ),
    "object": (
        "function.signature", "array.length", "array.elements",
        "growable_object_array.length", "growable_object_array.data", "string.length",
        "string.data", "object_pool.length", "object_pool.elements", "cids.class",
        "cids.function", "cids.library", "cids.code", "cids.object_pool", "cids.array",
        "cids.immutable_array", "cids.growable_object_array", "cids.one_byte_string",
        "cids.two_byte_string", "canonical_bool.thread_true",
        "canonical_bool.thread_false", "canonical_bool.value", "canonical_bool.cid",
        "function_type.abstract_type_flags", "function_type.type_parameters",
        "function_type.result_type", "function_type.parameter_types",
        "function_type.named_parameter_names", "function_type.packed_parameter_counts",
        "function_type.packed_type_parameter_counts", "function_type.cid_type",
        "function_type.cid_function_type", "function_type.cid_record_type",
        "function_type.cid_type_parameter", "function_type.cid_null",
        "function_type.cid_dynamic", "function_type.cid_void", "function_type.cid_never",
        "function_type.type_parameter_base", "function_type.type_parameter_index",
        "function_type.nullability_bits", "function_type.type_class_id_shift",
        "function_type.type_parameter_function_bit", "raw_object.heap_object_tag",
        "raw_object.smi_tag", "raw_object.smi_tag_mask", "raw_object.smi_tag_shift",
        "raw_object.class_id_tag_shift", "raw_object.class_id_tag_bits",
        "raw_object.compressed_word_size", "type_arguments.cid", "type_arguments.length",
        "type_arguments.types",
    ),
    "transition": (
        "thread.enter_safepoint_stub", "thread.exit_safepoint_stub",
        "thread.top_exit_frame", "thread.vm_tag", "thread.execution_state",
        "thread.exit_through_ffi", "transition.vm_tag_dart", "transition.execution_vm",
        "transition.execution_generated", "transition.execution_native",
        "transition.exit_none", "transition.exit_through_ffi",
        "transition.exit_through_runtime_call",
    ),
    "exception": (
        "thread.jump_to_frame_entry_point", "thread.active_exception",
        "thread.active_stacktrace",
    ),
}

# This is the single declaration of the profile fields consumed by each
# capability proof. The C++ appenders and mutation-coverage tests are derived
# from this table; keep it isomorphic with the readers in src/vm/abi/proof.cpp
# and adapters/flutter_vm/flutter_vm.cpp.
_RUNTIME_ROOT_FIELDS = (
    ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
    ("raw_object.smi_tag", "profile.raw_object.smi_tag"),
    ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
    ("raw_object.smi_tag_shift", "profile.raw_object.smi_tag_shift"),
    ("raw_object.class_id_tag_shift", "profile.raw_object.class_id_tag_shift"),
    ("raw_object.class_id_tag_bits", "profile.raw_object.class_id_tag_bits"),
    ("raw_object.compressed_word_size", "profile.raw_object.compressed_word_size"),
    ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
    ("thread.object_null", "profile.live_vm.thread_object_null_offset"),
    ("thread.global_object_pool", "profile.live_vm.thread_global_object_pool_offset"),
    ("thread.isolate", "profile.live_vm.thread_isolate_offset"),
    ("thread.isolate_group", "profile.live_vm.thread_isolate_group_offset"),
    ("isolate_group.class_table", "profile.live_vm.isolate_group_class_table_offset"),
    (
        "isolate_group.cached_class_table_table",
        "profile.live_vm.isolate_group_cached_class_table_table_offset",
    ),
    ("isolate_group.object_store", "profile.live_vm.isolate_group_object_store_offset"),
    ("class_table.num_cids", "profile.live_vm.class_table_num_cids_offset"),
    ("object_store.libraries", "profile.live_vm.object_store_libraries_offset"),
    ("array.length", "profile.live_vm.array_length_offset"),
    ("array.elements", "profile.live_vm.array_elements_offset"),
    ("growable_object_array.length", "profile.live_vm.growable_object_array_length_offset"),
    ("growable_object_array.data", "profile.live_vm.growable_object_array_data_offset"),
    ("library.url", "profile.live_vm.library_url_offset"),
    ("string.length", "profile.live_vm.string_length_offset"),
    ("string.data", "profile.live_vm.string_data_offset"),
    ("object_pool.length", "profile.live_vm.object_pool_length_offset"),
    ("cids.class", "profile.live_vm.cid_class"),
    ("cids.library", "profile.live_vm.cid_library"),
    ("cids.object_pool", "profile.live_vm.cid_object_pool"),
    ("cids.array", "profile.live_vm.cid_array"),
    ("cids.immutable_array", "profile.live_vm.cid_immutable_array"),
    ("cids.growable_object_array", "profile.live_vm.cid_growable_object_array"),
    ("cids.one_byte_string", "profile.live_vm.cid_one_byte_string"),
    ("cids.two_byte_string", "profile.live_vm.cid_two_byte_string"),
    ("function_type.cid_null", "profile.function_type.cid_null"),
)

_FUNCTION_CODE_FIELDS = (
    ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
    ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
    ("raw_object.compressed_word_size", "profile.raw_object.compressed_word_size"),
    ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
    ("function.code", "profile.live_vm.function_code_offset"),
    ("function.entry_point", "profile.live_vm.function_entry_point_offset"),
    ("function.unchecked_entry_point", "profile.live_vm.function_unchecked_entry_point_offset"),
    ("code.entry_point", "profile.live_vm.code_entry_point_offset"),
    ("code.unchecked_entry_point", "profile.live_vm.code_unchecked_entry_point_offset"),
    ("code.monomorphic_entry_point", "profile.live_vm.code_monomorphic_entry_point_offset"),
    (
        "code.monomorphic_unchecked_entry_point",
        "profile.live_vm.code_monomorphic_unchecked_entry_point_offset",
    ),
    ("code.owner", "profile.live_vm.code_owner_offset"),
    ("cids.function", "profile.live_vm.cid_function"),
    ("cids.code", "profile.live_vm.cid_code"),
)

CAPABILITY_FINGERPRINT_FIELDS = {
    "runtime_roots": _RUNTIME_ROOT_FIELDS,
    "owner_identity": (
        ("thread.isolate", "profile.live_vm.thread_isolate_offset"),
        ("thread.isolate_group", "profile.live_vm.thread_isolate_group_offset"),
    ),
    "canonical_null": (
        ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
        ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
        ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
        ("thread.object_null", "profile.live_vm.thread_object_null_offset"),
        ("function_type.cid_null", "profile.function_type.cid_null"),
    ),
    "register_semantics": (
        ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
        ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
        ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
        ("thread.global_object_pool", "profile.live_vm.thread_global_object_pool_offset"),
    ),
    "dart_core": _RUNTIME_ROOT_FIELDS,
    "safepoint_stubs": _RUNTIME_ROOT_FIELDS
    + (
        ("thread.enter_safepoint_stub", "profile.thread_bridge.enter_safepoint_stub_offset"),
        ("thread.exit_safepoint_stub", "profile.thread_bridge.exit_safepoint_stub_offset"),
        ("code.entry_point", "profile.live_vm.code_entry_point_offset"),
        ("cids.code", "profile.live_vm.cid_code"),
    ),
    "generated_transition_layout": (
        ("thread.top_exit_frame", "profile.thread_bridge.top_exit_frame_offset"),
        ("thread.vm_tag", "profile.thread_bridge.vm_tag_offset"),
        ("thread.execution_state", "profile.thread_bridge.execution_state_offset"),
        ("thread.exit_through_ffi", "profile.thread_bridge.exit_through_ffi_offset"),
        ("transition.vm_tag_dart", "profile.transition.vm_tag_dart"),
        ("transition.execution_vm", "profile.transition.execution_vm"),
        ("transition.execution_generated", "profile.transition.execution_generated"),
        ("transition.execution_native", "profile.transition.execution_native"),
        ("transition.exit_none", "profile.transition.exit_none"),
        ("transition.exit_through_ffi", "profile.transition.exit_through_ffi"),
        ("transition.exit_through_runtime_call", "profile.transition.exit_through_runtime_call"),
    ),
    "exception_layout": (
        ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
        ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
        ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
        ("thread.active_exception", "profile.thread_bridge.active_exception_offset"),
        ("thread.active_stacktrace", "profile.thread_bridge.active_stacktrace_offset"),
    ),
    "exception_bridge_layout": (
        ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
        ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
        ("raw_object.compressed_word_size", "profile.raw_object.compressed_word_size"),
        ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
        ("thread.jump_to_frame_entry_point", "profile.thread_jump_to_frame_entry_point_offset"),
        ("cids.code", "profile.live_vm.cid_code"),
        ("code.entry_point", "profile.live_vm.code_entry_point_offset"),
        ("code.owner", "profile.live_vm.code_owner_offset"),
    ),
    "type_arguments_layout": (
        ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
        ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
        ("raw_object.smi_tag_shift", "profile.raw_object.smi_tag_shift"),
        ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
        ("type_arguments.cid", "profile.type_arguments.cid"),
        ("type_arguments.length", "profile.type_arguments.length_offset"),
        ("type_arguments.types", "profile.type_arguments.types_offset"),
    ),
    "arguments_descriptor_layout": (
        ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
        ("raw_object.smi_tag", "profile.raw_object.smi_tag"),
        ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
        ("raw_object.smi_tag_shift", "profile.raw_object.smi_tag_shift"),
        ("raw_object.class_id_tag_shift", "profile.raw_object.class_id_tag_shift"),
        ("raw_object.class_id_tag_bits", "profile.raw_object.class_id_tag_bits"),
        ("raw_object.compressed_word_size", "profile.raw_object.compressed_word_size"),
        ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
        ("arguments_descriptor.type_args_len", "profile.arguments_descriptor.type_args_len_offset"),
        ("arguments_descriptor.count", "profile.arguments_descriptor.count_offset"),
        ("arguments_descriptor.size", "profile.arguments_descriptor.size_offset"),
        (
            "arguments_descriptor.positional_count",
            "profile.arguments_descriptor.positional_count_offset",
        ),
        (
            "arguments_descriptor.first_named_entry",
            "profile.arguments_descriptor.first_named_entry_offset",
        ),
        ("arguments_descriptor.named_entry_size", "profile.arguments_descriptor.named_entry_size"),
        ("arguments_descriptor.name", "profile.arguments_descriptor.name_offset"),
        ("arguments_descriptor.position", "profile.arguments_descriptor.position_offset"),
        ("cids.array", "profile.live_vm.cid_array"),
        ("cids.immutable_array", "profile.live_vm.cid_immutable_array"),
        ("array.length", "profile.live_vm.array_length_offset"),
        ("array.elements", "profile.live_vm.array_elements_offset"),
        ("cids.one_byte_string", "profile.live_vm.cid_one_byte_string"),
        ("cids.two_byte_string", "profile.live_vm.cid_two_byte_string"),
        ("string.length", "profile.live_vm.string_length_offset"),
        ("string.data", "profile.live_vm.string_data_offset"),
    ),
    "invocation_call_abi": _FUNCTION_CODE_FIELDS + (
        ("registers.args_desc", "profile.arguments_descriptor_register"),
    ),
    "function_code_layout": _FUNCTION_CODE_FIELDS,
    "aot_entry_layout": _FUNCTION_CODE_FIELDS + (
        ("instructions.monomorphic_entry_offset_aot", "profile.instructions_monomorphic_entry_offset_aot"),
        ("instructions.polymorphic_entry_offset_aot", "profile.instructions_polymorphic_entry_offset_aot"),
        ("code.instructions_length", "profile.live_vm.code_instructions_length_offset"),
    ),
    "function_type_layout": (
        ("raw_object.heap_object_tag", "profile.raw_object.heap_object_tag"),
        ("raw_object.smi_tag", "profile.raw_object.smi_tag"),
        ("raw_object.smi_tag_mask", "profile.raw_object.smi_tag_mask"),
        ("raw_object.smi_tag_shift", "profile.raw_object.smi_tag_shift"),
        ("raw_object.class_id_tag_shift", "profile.raw_object.class_id_tag_shift"),
        ("raw_object.class_id_tag_bits", "profile.raw_object.class_id_tag_bits"),
        ("raw_object.compressed_word_size", "profile.raw_object.compressed_word_size"),
        ("thread.heap_base", "profile.live_vm.thread_heap_base_offset"),
        ("function.signature", "profile.function_type.function_signature_offset"),
        ("array.length", "profile.live_vm.array_length_offset"),
        ("array.elements", "profile.live_vm.array_elements_offset"),
        ("string.length", "profile.live_vm.string_length_offset"),
        ("string.data", "profile.live_vm.string_data_offset"),
        ("cids.function", "profile.live_vm.cid_function"),
        ("cids.array", "profile.live_vm.cid_array"),
        ("cids.immutable_array", "profile.live_vm.cid_immutable_array"),
        ("cids.one_byte_string", "profile.live_vm.cid_one_byte_string"),
        ("cids.two_byte_string", "profile.live_vm.cid_two_byte_string"),
        ("function_type.abstract_type_flags", "profile.function_type.abstract_type_flags_offset"),
        ("function_type.result_type", "profile.function_type.result_type_offset"),
        ("function_type.parameter_types", "profile.function_type.parameter_types_offset"),
        ("function_type.named_parameter_names", "profile.function_type.named_parameter_names_offset"),
        ("function_type.packed_parameter_counts", "profile.function_type.packed_parameter_counts_offset"),
        (
            "function_type.packed_type_parameter_counts",
            "profile.function_type.packed_type_parameter_counts_offset",
        ),
        ("function_type.cid_type", "profile.function_type.cid_type"),
        ("function_type.cid_function_type", "profile.function_type.cid_function_type"),
        ("function_type.cid_record_type", "profile.function_type.cid_record_type"),
        ("function_type.cid_type_parameter", "profile.function_type.cid_type_parameter"),
        ("function_type.cid_null", "profile.function_type.cid_null"),
        ("function_type.cid_dynamic", "profile.function_type.cid_dynamic"),
        ("function_type.cid_void", "profile.function_type.cid_void"),
        ("function_type.cid_never", "profile.function_type.cid_never"),
        ("function_type.type_parameter_base", "profile.function_type.type_parameter_base_offset"),
        ("function_type.type_parameter_index", "profile.function_type.type_parameter_index_offset"),
        ("function_type.nullability_bits", "profile.function_type.nullability_bits"),
        ("function_type.type_class_id_shift", "profile.function_type.type_class_id_shift"),
        (
            "function_type.type_parameter_function_bit",
            "profile.function_type.type_parameter_function_bit",
        ),
    ),
}


def _abi_domain_payload(profile: dict[str, object], domain: str) -> dict[str, object]:
    """Return the explicit private-ABI facts owned by one compatibility domain."""

    payload: dict[str, object] = {}
    for path in ABI_DOMAIN_FIELDS[domain]:
        section, field = path.split(".")
        payload[path] = profile[section][field]
    return payload


def _abi_payload(profile: dict[str, object]) -> dict[str, object]:
    """Return all five canonical private-ABI domain payloads."""

    return {domain: _abi_domain_payload(profile, domain) for domain in ABI_DOMAIN_FIELDS}


def _canonical_abi_id(profile: dict[str, object], domain: str = "full") -> str:
    if domain == "full":
        abi_payload = _abi_payload(profile)
    else:
        if domain not in ABI_DOMAIN_FIELDS:
            raise ValueError(f"unknown VM ABI identity domain: {domain}")
        abi_payload = _abi_domain_payload(profile, domain)
    payload = json.dumps(abi_payload, sort_keys=True, separators=(",", ":")).encode()
    digest = hashlib.sha256(payload).hexdigest()[:24]
    machine = profile["machine"]
    architecture = str(machine["architecture"])
    mode = "product" if bool(machine["product"]) else "nonproduct"
    compression = "compressed" if bool(machine["compressed_pointers"]) else "uncompressed"
    domain_label = "" if domain == "full" else f"{domain}-"
    return f"dart-vm-{architecture}-{mode}-{compression}/abi-{domain_label}{digest}"


def _verify_abi_domain_coverage(profile: dict[str, object]) -> None:
    expected = {
        f"{section}.{field}"
        for section in ABI_PROFILE_SECTIONS
        for field in profile[section]
    }
    paths = [path for fields in ABI_DOMAIN_FIELDS.values() for path in fields]
    duplicates = sorted(path for path in set(paths) if paths.count(path) != 1)
    missing = sorted(expected - set(paths))
    unknown = sorted(set(paths) - expected)
    if duplicates or missing or unknown:
        raise ValueError(
            "VM ABI domain field coverage is invalid: "
            f"duplicates={duplicates}, missing={missing}, unknown={unknown}"
        )


def _verify_capability_fingerprint_coverage(profile: dict[str, object]) -> None:
    expected = {
        f"{section}.{field}"
        for section in ABI_PROFILE_SECTIONS
        for field in profile[section]
    }
    for capability, fields in CAPABILITY_FINGERPRINT_FIELDS.items():
        paths = [path for path, _ in fields]
        if len(paths) != len(set(paths)):
            raise ValueError(f"{capability}: capability fingerprint fields contain duplicates")
        unknown = sorted(set(paths) - expected)
        if unknown:
            raise ValueError(
                f"{capability}: capability fingerprint fields are unknown: {unknown}"
            )


def _verify_abi_layer_separation(
    profile: dict[str, object],
    *,
    domain_fields: dict[str, tuple[str, ...]] | None = None,
    capability_fields: dict[str, tuple[tuple[str, str], ...]] | None = None,
) -> None:
    domains = ABI_DOMAIN_FIELDS if domain_fields is None else domain_fields
    capabilities = CAPABILITY_FINGERPRINT_FIELDS if capability_fields is None else capability_fields

    for section in ABI_PROFILE_SECTIONS:
        for field in profile[section]:
            if field in ARTIFACT_ONLY_ABI_FIELD_NAMES:
                raise ValueError(
                    f"{section}.{field}: artifact/runtime fact must not enter RuntimeProfileRecord ABI"
                )

    def reject_path(owner: str, path: str) -> None:
        if path in PROVENANCE_ONLY_PROFILE_FIELDS:
            raise ValueError(f"{owner}: provenance-only field must not enter ABI identity: {path}")
        leaf = path.rsplit(".", 1)[-1]
        if leaf in ARTIFACT_ONLY_ABI_FIELD_NAMES:
            raise ValueError(f"{owner}: artifact/runtime field must not enter ABI identity: {path}")

    for domain, fields in domains.items():
        for path in fields:
            reject_path(f"ABI domain {domain}", path)
    for capability, fields in capabilities.items():
        for path, _ in fields:
            reject_path(f"capability {capability}", path)


def _verify_class_ids(profile: dict[str, object], class_id_text: str) -> None:
    actual = _class_id_map(class_id_text)
    cids = profile["cids"]
    function_type = profile["function_type"]
    expected = {
        "ClassCid": int(cids["class"]),
        "FunctionCid": int(cids["function"]),
        "LibraryCid": int(cids["library"]),
        "CodeCid": int(cids["code"]),
        "ObjectPoolCid": int(cids["object_pool"]),
        "ArrayCid": int(cids["array"]),
        "ImmutableArrayCid": int(cids["immutable_array"]),
        "GrowableObjectArrayCid": int(cids["growable_object_array"]),
        "OneByteStringCid": int(cids["one_byte_string"]),
        "TwoByteStringCid": int(cids["two_byte_string"]),
        "TypeArgumentsCid": int(profile["type_arguments"]["cid"]),
        "BoolCid": int(profile["canonical_bool"]["cid"]),
        "TypeCid": int(function_type["cid_type"]),
        "FunctionTypeCid": int(function_type["cid_function_type"]),
        "RecordTypeCid": int(function_type["cid_record_type"]),
        "TypeParameterCid": int(function_type["cid_type_parameter"]),
        "NullCid": int(function_type["cid_null"]),
        "DynamicCid": int(function_type["cid_dynamic"]),
        "VoidCid": int(function_type["cid_void"]),
        "NeverCid": int(function_type["cid_never"]),
    }
    for name, value in expected.items():
        if actual.get(name) != value:
            raise ValueError(
                f"{profile['name']}: manifest CID {name}={value} disagrees with Dart source "
                f"value {actual.get(name)}"
            )


def _verify_transition_constants(
    profile: dict[str, object], thread_text: str, tags_text: str, source_name: str
) -> None:
    transition = profile["transition"]
    expected = {
        "execution_vm": 0,
        "execution_generated": 1,
        "execution_native": 2,
        "exit_none": 0,
        "exit_through_ffi": 1,
        "exit_through_runtime_call": 2,
    }
    for field, value in expected.items():
        if int(transition[field]) != value:
            raise ValueError(
                f"{profile['name']}: transition {field}={transition[field]} "
                f"disagrees with {source_name} value {value}"
            )

    normalized_thread = " ".join(thread_text.split())
    execution_evidence = (
        "enum ExecutionState { kThreadInVM = 0, kThreadInGenerated, kThreadInNative,"
    )
    if execution_evidence not in normalized_thread:
        raise ValueError(f"{source_name}: Dart Thread ExecutionState numbering changed")
    for evidence in (
        "kDidNotExit = 0,",
        "kExitThroughFfi = 1,",
        "kExitThroughRuntimeCall = 2,",
    ):
        if evidence not in thread_text:
            raise ValueError(f"{source_name}: Dart Thread exit marker changed: {evidence}")

    vm_tags = _extract_v_macro(tags_text, "VM_TAG_LIST")
    try:
        dart_tag = vm_tags.index("Dart") + 1  # kInvalidTagId occupies zero.
    except ValueError as error:
        raise ValueError(f"{source_name}: Dart VM tag is missing") from error
    if int(transition["vm_tag_dart"]) != dart_tag:
        raise ValueError(
            f"{profile['name']}: vm_tag_dart={transition['vm_tag_dart']} disagrees with "
            f"{source_name} value {dart_tag}"
        )


def _verify_function_kinds(
    profile: dict[str, object], raw_object_text: str, object_header_text: str
) -> None:
    kinds = _extract_v_macro(raw_object_text, "FOR_EACH_RAW_FUNCTION_KIND")
    expected = profile["function_kind"]
    for field, source_name in (
        ("regular", "RegularFunction"),
        ("closure", "ClosureFunction"),
        ("implicit_closure", "ImplicitClosureFunction"),
    ):
        try:
            actual = kinds.index(source_name)
        except ValueError as error:
            raise ValueError(f"Dart Function kind is missing: {source_name}") from error
        if actual != int(expected[field]):
            raise ValueError(
                f"{profile['name']}: Function kind {source_name}={actual} disagrees with "
                f"manifest {expected[field]}"
            )
    expected_bits = max(1, (len(kinds) - 1).bit_length())
    if int(expected["tag_shift"]) != 0 or int(expected["tag_bits"]) != expected_bits:
        raise ValueError(
            f"{profile['name']}: Function kind tag layout 0/{expected_bits} disagrees with manifest"
        )
    normalized = " ".join(object_header_text.split())
    modern_evidence = (
        "using KindBits = BitField<decltype(UntaggedFunction::kind_tag_), "
        "UntaggedFunction::Kind, 0, UntaggedFunction::kKindBitSize>;"
    )
    if modern_evidence in normalized:
        return

    # Dart 3.4/3.5 spell the same field through enum constants and a derived
    # BitField class. Verify both the concrete constants and their use by
    # KindBits instead of assuming the historical spelling is equivalent.
    old_position = re.search(r"\bkKindTagPos\s*=\s*([0-9]+)\b", object_header_text)
    old_bits = re.search(r"\bkKindTagSize\s*=\s*([0-9]+)\b", object_header_text)
    old_kind_bits = (
        "class KindBits : public BitField<uint32_t, UntaggedFunction::Kind, "
        "kKindTagPos, kKindTagSize> {};"
    )
    if (
        old_position is None
        or old_bits is None
        or int(old_position.group(1)) != int(expected["tag_shift"])
        or int(old_bits.group(1)) != int(expected["tag_bits"])
        or old_kind_bits not in normalized
    ):
        raise ValueError("Dart Function::KindBits no longer matches the verified kind_tag layout")


def _parse_source_integer(text: str, name: str) -> int:
    match = re.search(rf"\b{re.escape(name)}\s*=\s*([0-9]+)\b", text)
    if match is None:
        raise ValueError(f"Dart source integer is missing: {name}")
    return int(match.group(1))


def _verify_raw_object_layout(
    profile: dict[str, object], pointer_tagging: str, raw_object: str, runtime_api: str,
    platform_globals: str, source_name: str
) -> None:
    expected = profile["raw_object"]
    for field, source_name_part in (
        ("heap_object_tag", "kHeapObjectTag"),
        ("smi_tag", "kSmiTag"),
        ("smi_tag_mask", "kSmiTagMask"),
        ("smi_tag_shift", "kSmiTagShift"),
    ):
        actual = _parse_source_integer(pointer_tagging, source_name_part)
        if actual != int(expected[field]):
            raise ValueError(
                f"{profile['name']}: raw-object {field}={expected[field]} disagrees with "
                f"{source_name} {source_name_part}={actual}"
            )

    normalized_raw = " ".join(raw_object.split())
    size_tag = re.search(
        r"using SizeTagBits = BitField<[^;]+, kBitsPerInt8, ([0-9]+)>;",
        normalized_raw,
    )
    class_tag = re.search(
        r"using ClassIdTag = BitField<[^;]+, ClassIdTagType, SizeTagBits::kNextBit, ([0-9]+)>;",
        normalized_raw,
    )
    if size_tag is not None and class_tag is not None:
        class_id_shift = 8 + int(size_tag.group(1))
        class_id_bits = int(class_tag.group(1))
    else:
        # Dart <= 3.4 spells the same layout through explicit enum constants
        # rather than nested BitField aliases. Prefer the source's evaluated
        # comment for the position so this still proves the expression rather
        # than silently assuming 8 + 4.
        old_shift = re.search(
            r"kClassIdTagPos\s*=\s*[^,\n]+,\s*//\s*=\s*([0-9]+)", raw_object
        )
        old_bits = re.search(r"\bkClassIdTagSize\s*=\s*([0-9]+)\b", raw_object)
        if old_shift is None or old_bits is None:
            raise ValueError(f"{source_name}: UntaggedObject ClassIdTag layout changed")
        class_id_shift = int(old_shift.group(1))
        class_id_bits = int(old_bits.group(1))
    if class_id_shift != int(expected["class_id_tag_shift"]) or class_id_bits != int(
        expected["class_id_tag_bits"]
    ):
        raise ValueError(
            f"{profile['name']}: ClassIdTag layout {class_id_shift}/{class_id_bits} disagrees "
            f"with {source_name}"
        )

    int32_log2 = _parse_source_integer(platform_globals, "kInt32SizeLog2")
    compressed_word_size = 1 << int32_log2
    normalized_runtime = " ".join(runtime_api.split())
    if (
        "#if defined(DART_COMPRESSED_POINTERS) static constexpr intptr_t kCompressedWordSize = "
        "kInt32Size;" not in normalized_runtime
    ):
        raise ValueError(f"{source_name}: compressed pointer word-size contract changed")
    if compressed_word_size != int(expected["compressed_word_size"]):
        raise ValueError(
            f"{profile['name']}: compressed_word_size={expected['compressed_word_size']} disagrees "
            f"with {source_name} kInt32Size={compressed_word_size}"
        )


def _verify_calling_convention_stack_layout(
    calling_convention: str, locations: str, source_name: str
) -> None:
    calling = " ".join(calling_convention.split())
    for evidence in (
        "if (i < max_arguments_in_registers)",
        "intptr_t offset_in_words_from_fp = offset_to_last_parameter_slot_from_fp;",
        "for (intptr_t i = argc - 1; i >= 0; --i)",
        "offset_to_last_parameter_slot_from_fp = (compiler::target::frame_layout.param_end_from_fp + 1)",
    ):
        if evidence not in calling:
            raise ValueError(f"{source_name}: Dart stack calling-convention contract changed: {evidence}")
    location_text = " ".join(locations.split())
    for evidence in (
        "const auto fp_to_entry_sp_delta = (compiler::target::frame_layout.param_end_from_fp + 1) - compiler::target::frame_layout.last_param_from_entry_sp;",
        "return ToSpRelative(fp_to_entry_sp_delta);",
    ):
        if evidence not in location_text:
            raise ValueError(f"{source_name}: Dart entry-SP location mapping changed: {evidence}")


def _verify_generated_native_transition_contract(
    assembler_arm64: str,
    stack_frame_arm64: str,
    stack_frame: str,
    dart_api_impl: str,
    source_name: str,
) -> None:
    transition_start = assembler_arm64.find("void Assembler::TransitionGeneratedToNative")
    reverse_start = assembler_arm64.find("void Assembler::TransitionNativeToGenerated")
    if transition_start < 0 or reverse_start < 0:
        raise ValueError(f"{source_name}: ARM64 generated/native transition implementation is missing")
    transition = " ".join(assembler_arm64[transition_start : transition_start + 2600].split())
    reverse = " ".join(assembler_arm64[reverse_start : reverse_start + 2600].split())
    for evidence in (
        "StoreToOffset(new_exit_frame, THR, target::Thread::top_exit_frame_info_offset());",
        "StoreToOffset(new_exit_through_ffi, THR, target::Thread::exit_through_ffi_offset());",
        "StoreToOffset(destination, THR, target::Thread::vm_tag_offset());",
        "LoadImmediate(tmp, target::Thread::native_execution_state());",
        "StoreToOffset(tmp, THR, target::Thread::execution_state_offset());",
        "EnterFullSafepoint(tmp);",
    ):
        if evidence not in transition:
            raise ValueError(f"{source_name}: Generated->Native contract changed: {evidence}")
    for evidence in (
        "ExitFullSafepoint(state",
        "LoadImmediate(state, target::Thread::generated_execution_state());",
        "StoreToOffset(state, THR, target::Thread::execution_state_offset());",
        "StoreToOffset(ZR, THR, target::Thread::top_exit_frame_info_offset());",
        "StoreToOffset(state, THR, target::Thread::exit_through_ffi_offset());",
    ):
        if evidence not in reverse:
            raise ValueError(f"{source_name}: Native->Generated contract changed: {evidence}")

    frame_layout = " ".join(stack_frame_arm64.split())
    for evidence in (
        "kFirstObjectSlotFromFp = -1;",
        "kLastFixedObjectSlotFromFp = -2;",
        "kSavedCallerFpSlotFromFp = 0;",
        "kSavedCallerPcSlotFromFp = 1;",
        "kCallerSpSlotFromFp = 2;",
    ):
        if evidence not in frame_layout:
            raise ValueError(f"{source_name}: ARM64 synthetic ExitFrame layout changed: {evidence}")
    iterator = " ".join(stack_frame.split())
    if (
        "uword exit_marker = thread_->top_exit_frame_info();" not in iterator
        or "frames_.fp_ = exit_marker;" not in iterator
    ):
        raise ValueError(f"{source_name}: StackFrameIterator top-exit-frame contract changed")

    api = " ".join(dart_api_impl.split())
    scope_start = api.find("DART_EXPORT void Dart_EnterScope()")
    scope = api[scope_start : scope_start + 600] if scope_start >= 0 else ""
    if "TransitionNativeToVM transition(thread);" not in scope or "thread->EnterApiScope();" not in scope:
        raise ValueError(f"{source_name}: Dart_EnterScope no longer requires Native->VM transition")


def _verify_arm64_return_frame_identity(
    assembler_arm64: str, il_arm64: str, source_name: str
) -> None:
    """Verify the frame identity used by payload-level RET ownership.

    DartPlant matches an intercepted return against the entry-time LR, SPREG
    and caller FP. On ARM64 this is sound only while framed Dart returns restore
    R15/FP/LR before RET and frameless returns leave those values untouched.
    Keep this as an SDK source contract rather than a runtime assumption.
    """

    def function_body(source: str, marker: str, span: int) -> str:
        start = source.find(marker)
        if start < 0:
            raise ValueError(f"{source_name}: ARM64 return-frame implementation is missing: {marker}")
        return " ".join(source[start : start + span].split())

    enter_frame = function_body(assembler_arm64, "void Assembler::EnterFrame", 700)
    if re.search(
        r"SPILLS_LR_TO_FRAME\(PushPair\(FP, LR\)\);.*?mov\(FP, SP\);",
        enter_frame,
    ) is None:
        raise ValueError(f"{source_name}: ARM64 Dart frame no longer saves caller FP/LR before changing FP")

    leave_frame = function_body(assembler_arm64, "void Assembler::LeaveFrame", 350)
    if re.search(
        r"mov\(SP, FP\);.*?RESTORES_LR_FROM_FRAME\(PopPair\(FP, LR\)\);",
        leave_frame,
    ) is None:
        raise ValueError(f"{source_name}: ARM64 Dart frame no longer restores entry SPREG/caller FP/LR")

    enter_dart = function_body(assembler_arm64, "void Assembler::EnterDartFrame", 650)
    if "EnterFrame(0);" not in enter_dart:
        raise ValueError(f"{source_name}: ARM64 EnterDartFrame no longer uses the verified frame layout")
    leave_dart = function_body(assembler_arm64, "void Assembler::LeaveDartFrame", 650)
    if "LeaveFrame();" not in leave_dart:
        raise ValueError(f"{source_name}: ARM64 LeaveDartFrame no longer restores the verified frame layout")

    dart_return = function_body(il_arm64, "void DartReturnInstr::EmitNativeCode", 4200)
    if re.search(
        r"if \(!compiler->flow_graph\(\)\.graph_entry\(\)->NeedsFrame\(\)\) \{\s*__ ret\(\);\s*return;\s*\}",
        dart_return,
    ) is None:
        raise ValueError(f"{source_name}: ARM64 frameless Dart return contract changed")
    if re.search(r"__ LeaveDartFrame\(\);.*?__ ret\(\);", dart_return) is None:
        raise ValueError(f"{source_name}: ARM64 framed Dart return no longer restores its caller frame before RET")


def _verify_closure_stack_contract(function_impl: str, dart_entry: str, source_name: str) -> None:
    start = function_impl.find("Function::MaxNumberOfParametersInRegisters")
    if start < 0:
        raise ValueError(f"{source_name}: register-CC Function policy is missing")
    body = " ".join(function_impl[start : start + 5000].split())
    if re.search(
        r"case UntaggedFunction::kClosureFunction: .*?"
        r"case UntaggedFunction::kImplicitClosureFunction: .*?return 0;",
        body,
    ) is None:
        raise ValueError(f"{source_name}: closures no longer force stack calling convention")
    descriptor = " ".join(dart_entry.split())
    for evidence in (
        "Right now this is for example the case for all closure functions.",
        "return New(type_args_len, num_arguments, num_arguments,",
    ):
        if evidence not in descriptor:
            raise ValueError(f"{source_name}: boxed closure ArgumentsDescriptor contract changed")


def _verify_closure_call_descriptor_contract(
    il_header: str, kernel_flowgraph: str, source_name: str
) -> None:
    text = " ".join(il_header.split())
    for evidence in (
        "class ClosureCallInstr : public TemplateDartCall<1>",
        "return move_arguments_ != nullptr ? move_arguments_->length() : InputCount() - kExtraInputs;",
        "intptr_t FirstArgIndex() const { return type_args_len_ > 0 ? 1 : 0; }",
        "return ArgumentsDescriptor::New( type_args_len(), ArgumentCountWithoutTypeArgs(), ArgumentsSizeWithoutTypeArgs(), argument_names());",
    ):
        if evidence not in text:
            raise ValueError(
                f"{source_name}: ClosureCall ArgumentsDescriptor input accounting changed: {evidence}"
            )
    flowgraph = " ".join(kernel_flowgraph.split())
    for evidence in (
        "++argument_count; // include receiver",
        "B->ClosureCall(target_function, position, type_args_len, argument_count, argument_names, &result_type);",
    ):
        if evidence not in flowgraph:
            raise ValueError(
                f"{source_name}: ClosureCall hidden-receiver accounting changed: {evidence}"
            )


def _verify_snapshot_header_contract(
    snapshot_header: str, source_name: str, expected_full_aot_kind: int | None = 3
) -> None:
    normalized = " ".join(snapshot_header.split())
    required = (
        "static constexpr int32_t kMagicValue = 0xdcdcf5f5;",
        "static constexpr intptr_t kMagicOffset = 0;",
        "static constexpr intptr_t kMagicSize = sizeof(int32_t);",
        "static constexpr intptr_t kLengthOffset = kMagicOffset + kMagicSize;",
        "static constexpr intptr_t kLengthSize = sizeof(int64_t);",
        "static constexpr intptr_t kKindOffset = kLengthOffset + kLengthSize;",
        "static constexpr intptr_t kKindSize = sizeof(int64_t);",
        "static constexpr intptr_t kHeaderSize = kKindOffset + kKindSize;",
        "return Read<int64_t>(kLengthOffset) + kMagicSize;",
        "return Write<int64_t>(kLengthOffset, value - kMagicSize);",
    )
    for evidence in required:
        if evidence not in normalized:
            raise ValueError(f"{source_name}: Dart snapshot header contract changed: {evidence}")

    enum_match = re.search(r"enum\s+Kind\s*\{(?P<body>.*?)\};", snapshot_header, re.DOTALL)
    if enum_match is None:
        raise ValueError(f"{source_name}: Dart Snapshot::Kind enum is missing")
    values: dict[str, int] = {}
    current = -1
    for raw in enum_match.group("body").split(","):
        token = re.sub(r"//.*", "", raw).strip()
        if not token:
            continue
        if "=" in token:
            name, value = [part.strip() for part in token.split("=", 1)]
            current = int(value, 0)
        else:
            name = token
            current += 1
        values[name] = current
    if "kFullAOT" not in values:
        raise ValueError(f"{source_name}: Snapshot::kFullAOT is missing")
    if expected_full_aot_kind is not None and values.get("kFullAOT") != expected_full_aot_kind:
        raise ValueError(
            f"{source_name}: Snapshot::kFullAOT changed: {values.get('kFullAOT')!r}"
        )


def _verify_snapshot_symbol_contract(
    dart_api: str,
    source_name: str,
    expected_symbols: dict[str, str] | None = EXPECTED_SNAPSHOT_SYMBOLS,
) -> None:
    normalized = " ".join(dart_api.replace("\\\n", " ").split())
    if expected_symbols is None:
        current_macros = (
            "kSnapshotBuildIdAsmSymbol",
            "kSnapshotDataAsmSymbol",
            "kSnapshotTextAsmSymbol",
            "kSnapshotBssAsmSymbol",
        )
        for macro in current_macros:
            if re.search(rf"#define\s+{re.escape(macro)}\s+\"[^\"]+\"", normalized) is None:
                raise ValueError(f"{source_name}: current Dart AOT snapshot symbol is missing: {macro}")
        return
    for macro, value in expected_symbols.items():
        pattern = rf'#define\s+{re.escape(macro)}\s+"{re.escape(value)}"'
        if re.search(pattern, normalized) is None:
            raise ValueError(f"{source_name}: Dart AOT snapshot symbol changed: {macro}")


def _verify_snapshot_feature_contract(dart_source: str, source_name: str) -> None:
    normalized = " ".join(dart_source.split())
    for evidence in (
        '#if defined(DEBUG) buffer.AddString("debug"); #elif defined(PRODUCT) buffer.AddString("product"); #else buffer.AddString("release"); #endif',
        '#elif defined(TARGET_ARCH_ARM64) buffer.AddString(" arm64");',
        '#if defined(DART_TARGET_OS_ANDROID) buffer.AddString(" android");',
        '#if defined(DART_COMPRESSED_POINTERS) buffer.AddString(" compressed-pointers"); #else buffer.AddString(" no-compressed-pointers"); #endif',
    ):
        if evidence not in normalized:
            raise ValueError(f"{source_name}: Dart snapshot feature contract changed: {evidence}")


def _verify_parameter_flags_contract(runtime_api: str, platform_globals: str, source_name: str) -> None:
    api = " ".join(runtime_api.split())
    globals_text = " ".join(platform_globals.split())
    enum_match = re.search(r"enum ParameterFlags \{ (?P<body>.*?) \};", api)
    if enum_match is None:
        raise ValueError(f"{source_name}: ParameterFlags enum is missing")
    names = [part.strip() for part in enum_match.group("body").split(",") if part.strip()]
    if names[:2] != ["kRequiredNamedParameterFlag", "kNumParameterFlags"]:
        raise ValueError(f"{source_name}: named-parameter flag numbering changed: {names[:2]}")
    for evidence in (
        "kNumParameterFlagsPerElementLog2 = kBitsPerWordLog2 - 1 - kNumParameterFlags;",
        "kNumParameterFlagsPerElement = 1 << kNumParameterFlagsPerElementLog2;",
    ):
        if evidence not in api:
            raise ValueError(f"{source_name}: named-parameter flag packing changed: {evidence}")
    for evidence in (
        "constexpr intptr_t kInt64SizeLog2 = 3;",
        "constexpr intptr_t kBitsPerByteLog2 = 3;",
        "constexpr intptr_t kWordSizeLog2 = kInt64SizeLog2;",
        "constexpr intptr_t kBitsPerWordLog2 = kWordSizeLog2 + kBitsPerByteLog2;",
    ):
        if evidence not in globals_text:
            raise ValueError(f"{source_name}: 64-bit word geometry changed: {evidence}")
    # ARM64 is a 64-bit target: 1 << (6 - 1 - 1) == 16.
    if 1 << ((3 + 3) - 1 - 1) != 16:
        raise AssertionError("internal named-parameter packing derivation is invalid")


def _verify_dart_aot_elf_contract(elf_source: str, source_name: str) -> None:
    normalized = " ".join(elf_source.split())
    for evidence in (
        "DynamicEntryType::DT_HASH",
        'section_table_->Add(hash, ".hash");',
        "GenerateBuildId();",
        "write_section(section_table_);",
    ):
        if evidence not in normalized:
            raise ValueError(f"{source_name}: Dart AOT ELF producer contract changed: {evidence}")


def _snapshot_hash_inputs(make_version_source: str, source_name: str) -> list[str]:
    match = re.search(
        r"VM_SNAPSHOT_FILES\s*=\s*(\[[\s\S]*?\])\s*\n\s*\n",
        make_version_source,
    )
    if match is None:
        raise ValueError(f"{source_name}: tools/make_version.py snapshot input list is missing")
    try:
        values = ast.literal_eval(match.group(1))
    except (SyntaxError, ValueError) as error:
        raise ValueError(f"{source_name}: invalid VM_SNAPSHOT_FILES list") from error
    if not isinstance(values, list) or not values or not all(isinstance(value, str) for value in values):
        raise ValueError(f"{source_name}: invalid VM_SNAPSHOT_FILES entries")
    normalized = " ".join(make_version_source.split())
    for evidence in ("vmhash = hashlib.md5()", "return vmhash.hexdigest()"):
        if evidence not in normalized:
            raise ValueError(f"{source_name}: snapshot source hash algorithm changed: {evidence}")
    return values


def _git_show_bytes(sdk_root: Path, revision: str, path: str) -> bytes:
    result = sp.run(
        ["git", "-C", str(sdk_root), "show", f"{revision}:{path}"],
        check=False,
        stdout=sp.PIPE,
        stderr=sp.PIPE,
    )
    if result.returncode != 0:
        raise ValueError(
            f"Dart SDK revision {revision} cannot provide {path}: "
            f"{result.stderr.decode(errors='replace').strip()}"
        )
    return result.stdout


def _verify_snapshot_source_hash(
    sdk_root: Path,
    revision: str,
    make_version_source: str,
    expected_hash: str,
    source_name: str,
) -> None:
    inputs = _snapshot_hash_inputs(make_version_source, source_name)
    digest = hashlib.md5()
    for filename in inputs:
        digest.update(_git_show_bytes(sdk_root, revision, f"runtime/vm/{filename}"))
    actual = digest.hexdigest()
    if len(actual) != 32 or any(character not in "0123456789abcdef" for character in actual):
        raise ValueError(f"{source_name}: generated snapshot source hash is not 32 lowercase hex")
    if actual != expected_hash:
        raise ValueError(
            f"{source_name}: manifest snapshot hash {expected_hash} != source hash {actual}"
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
        raw_object = _git_show(sdk_root, version, "runtime/vm/raw_object.h")
        class_table_header = _git_show(sdk_root, version, "runtime/vm/class_table.h")
        class_id = _git_show(sdk_root, version, "runtime/vm/class_id.h")
        tags = _git_show(sdk_root, version, "runtime/vm/tags.h")
        thread = _git_show(sdk_root, version, "runtime/vm/thread.h")
        function_impl = _git_show(sdk_root, version, "runtime/vm/object.cc")
        dart_entry = _git_show(sdk_root, version, "runtime/vm/dart_entry.h")
        pointer_tagging = _git_show(sdk_root, version, "runtime/vm/pointer_tagging.h")
        runtime_api = _git_show(sdk_root, version, "runtime/vm/compiler/runtime_api.h")
        platform_globals = _git_show(sdk_root, version, "runtime/platform/globals.h")
        calling_convention = _git_show(
            sdk_root, version, "runtime/vm/compiler/backend/dart_calling_conventions.cc"
        )
        locations = _git_show(sdk_root, version, "runtime/vm/compiler/backend/locations.cc")
        assembler_arm64 = _git_show(
            sdk_root, version, "runtime/vm/compiler/assembler/assembler_arm64.cc"
        )
        il_arm64 = _git_show(
            sdk_root, version, "runtime/vm/compiler/backend/il_arm64.cc"
        )
        il_header = _git_show(sdk_root, version, "runtime/vm/compiler/backend/il.h")
        kernel_flowgraph = _git_show(
            sdk_root, version, "runtime/vm/compiler/frontend/kernel_binary_flowgraph.cc"
        )
        stack_frame_arm64 = _git_show(sdk_root, version, "runtime/vm/stack_frame_arm64.h")
        stack_frame = _git_show(sdk_root, version, "runtime/vm/stack_frame.cc")
        dart_api_impl = _git_show(sdk_root, version, "runtime/vm/dart_api_impl.cc")
        snapshot_header = _git_show(sdk_root, version, "runtime/vm/snapshot.h")
        dart_api = _git_show(sdk_root, version, "runtime/include/dart_api.h")
        dart_source = _git_show(sdk_root, version, "runtime/vm/dart.cc")
        elf_source = _git_show(sdk_root, version, "runtime/vm/elf.cc")
        make_version = _git_show(sdk_root, version, "tools/make_version.py")
        _verify_profile_against_aot_offsets(profile, runtime_offsets)
        _verify_class_table_num_cids_contract(
            profile,
            runtime_offsets,
            class_table_header,
            source_name=f"Dart SDK {version}",
        )
        _verify_class_raw_layout_contract(
            profile, raw_object, source_name=f"Dart SDK {version}"
        )
        _verify_code_instructions_length_contract(
            profile, raw_object, source_name=f"Dart SDK {version}"
        )
        _verify_aot_payload_contract(
            object_header, app_snapshot, source_name=f"Dart SDK {version}"
        )
        _verify_function_kinds(profile, raw_object, object_header)
        _verify_class_ids(profile, class_id)
        _verify_transition_constants(
            profile, thread, tags, source_name=f"Dart SDK {version}"
        )
        _verify_raw_object_layout(
            profile, pointer_tagging, raw_object, runtime_api, platform_globals,
            source_name=f"Dart SDK {version}"
        )
        _verify_calling_convention_stack_layout(
            calling_convention, locations, source_name=f"Dart SDK {version}"
        )
        _verify_generated_native_transition_contract(
            assembler_arm64,
            stack_frame_arm64,
            stack_frame,
            dart_api_impl,
            source_name=f"Dart SDK {version}",
        )
        _verify_arm64_return_frame_identity(
            assembler_arm64, il_arm64, source_name=f"Dart SDK {version}"
        )
        _verify_closure_stack_contract(
            function_impl, dart_entry, source_name=f"Dart SDK {version}"
        )
        _verify_closure_call_descriptor_contract(
            il_header, kernel_flowgraph, source_name=f"Dart SDK {version}"
        )
        _verify_snapshot_header_contract(snapshot_header, source_name=f"Dart SDK {version}")
        _verify_snapshot_symbol_contract(dart_api, source_name=f"Dart SDK {version}")
        _verify_snapshot_feature_contract(dart_source, source_name=f"Dart SDK {version}")
        _verify_parameter_flags_contract(
            runtime_api, platform_globals, source_name=f"Dart SDK {version}"
        )
        _verify_dart_aot_elf_contract(elf_source, source_name=f"Dart SDK {version}")
        _verify_snapshot_source_hash(
            sdk_root,
            version,
            make_version,
            str(profile["snapshot_hash"]),
            source_name=f"Dart SDK {version}",
        )
        print(f"Dart SDK AOT profile verified: {profile['name']} @ {version}")


def _load_manifest(path: Path = MANIFEST) -> list[dict[str, object]]:
    payload = json.loads(path.read_text())
    if payload.get("schema_version") != 3:
        raise ValueError("dart VM profile manifest schema_version must be 3")
    profiles = payload.get("profiles")
    if not isinstance(profiles, list) or not profiles:
        raise ValueError("dart VM profile manifest must contain profiles")

    versions: set[int] = set()
    source_identities: set[tuple[str, str]] = set()
    names: set[str] = set()
    for profile in profiles:
        version = int(profile["profile_version"])
        name = str(profile["name"])
        snapshot_hash = str(profile["snapshot_hash"])
        snapshot_profile = str(profile["snapshot_profile"])
        source_identity = (snapshot_hash, snapshot_profile)
        if version in versions or name in names or source_identity in source_identities:
            raise ValueError("dart VM profile identities must be unique")
        versions.add(version)
        names.add(name)
        source_identities.add(source_identity)
        if len(snapshot_hash) != 32 or any(c not in "0123456789abcdef" for c in snapshot_hash):
            raise ValueError(f"invalid snapshot hash for {name}: {snapshot_hash}")

        machine = profile["machine"]
        if (
            machine.get("architecture") != "arm64"
            or int(machine.get("pointer_size", 0)) != 8
            or not isinstance(machine.get("product"), bool)
            or machine.get("compressed_pointers") is not True
        ):
            raise ValueError(f"{name}: unsupported VM machine ABI")
        _verify_abi_domain_coverage(profile)
        _verify_capability_fingerprint_coverage(profile)
        _verify_abi_layer_separation(profile)
        expected_identities = {
            domain: _canonical_abi_id(profile, domain)
            for domain in ("full", *ABI_DOMAIN_FIELDS)
        }
        identities = profile.get("abi_identity")
        if identities != expected_identities:
            raise ValueError(
                f"{name}: abi_identity is stale: {identities} != {expected_identities}"
            )
        if profile.get("abi_id") != expected_identities["full"]:
            raise ValueError(
                f"{name}: abi_id is stale: {profile.get('abi_id')} != "
                f"{expected_identities['full']}"
            )

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
        arguments_descriptor = profile["arguments_descriptor"]
        if (
            int(arguments_descriptor["type_args_len"]) <= 0
            or int(arguments_descriptor["count"]) <= int(arguments_descriptor["type_args_len"])
            or int(arguments_descriptor["size"]) <= int(arguments_descriptor["count"])
            or int(arguments_descriptor["positional_count"])
            <= int(arguments_descriptor["size"])
            or int(arguments_descriptor["first_named_entry"])
            <= int(arguments_descriptor["positional_count"])
            or int(arguments_descriptor["named_entry_size"]) <= 0
        ):
            raise ValueError(f"{name}: invalid ArgumentsDescriptor layout")
        function_kind = profile["function_kind"]
        if [
            int(function_kind["regular"]),
            int(function_kind["closure"]),
            int(function_kind["implicit_closure"]),
        ] != [0, 1, 2]:
            raise ValueError(f"{name}: unsupported Dart Function kind numbering")
        if int(function_kind["tag_shift"]) != 0 or int(function_kind["tag_bits"]) <= 0:
            raise ValueError(f"{name}: invalid Dart Function kind tag layout")
        raw_object = profile["raw_object"]
        if (
            int(raw_object["heap_object_tag"]) != 1
            or int(raw_object["smi_tag"]) != 0
            or int(raw_object["smi_tag_mask"]) != 1
            or int(raw_object["smi_tag_shift"]) != 1
            or int(raw_object["class_id_tag_shift"]) <= 0
            or int(raw_object["class_id_tag_bits"]) <= 0
            or int(raw_object["compressed_word_size"]) not in (4, 8)
        ):
            raise ValueError(f"{name}: invalid raw tagged-object layout")
        type_arguments = profile["type_arguments"]
        if (
            int(type_arguments["cid"]) <= 0
            or int(type_arguments["length"]) <= 0
            or int(type_arguments["types"]) <= int(type_arguments["length"])
        ):
            raise ValueError(f"{name}: invalid TypeArguments layout")
        transition = profile["transition"]
        if (
            int(transition["execution_vm"]) != 0
            or int(transition["execution_generated"]) != 1
            or int(transition["execution_native"]) != 2
            or int(transition["exit_none"]) != 0
            or int(transition["exit_through_ffi"]) != 1
            or int(transition["exit_through_runtime_call"]) != 2
            or int(transition["vm_tag_dart"]) <= 0
        ):
            raise ValueError(f"{name}: invalid generated/native transition constants")
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
    tags = sdk_root / "runtime" / "vm" / "tags.h"
    object_header = sdk_root / "runtime" / "vm" / "object.h"
    dart_entry = sdk_root / "runtime" / "vm" / "dart_entry.h"
    pointer_tagging = sdk_root / "runtime" / "vm" / "pointer_tagging.h"
    runtime_api = sdk_root / "runtime" / "vm" / "compiler" / "runtime_api.h"
    platform_globals = sdk_root / "runtime" / "platform" / "globals.h"
    locations = sdk_root / "runtime" / "vm" / "compiler" / "backend" / "locations.cc"
    assembler_arm64 = (
        sdk_root / "runtime" / "vm" / "compiler" / "assembler" / "assembler_arm64.cc"
    )
    stack_frame_arm64 = sdk_root / "runtime" / "vm" / "stack_frame_arm64.h"
    stack_frame = sdk_root / "runtime" / "vm" / "stack_frame.cc"
    dart_api_impl = sdk_root / "runtime" / "vm" / "dart_api_impl.cc"
    il_arm64 = sdk_root / "runtime" / "vm" / "compiler" / "backend" / "il_arm64.cc"
    il_header = sdk_root / "runtime" / "vm" / "compiler" / "backend" / "il.h"
    kernel_flowgraph = (
        sdk_root
        / "runtime"
        / "vm"
        / "compiler"
        / "frontend"
        / "kernel_binary_flowgraph.cc"
    )
    snapshot_header = sdk_root / "runtime" / "vm" / "snapshot.h"
    dart_api = sdk_root / "runtime" / "include" / "dart_api.h"
    dart_source = sdk_root / "runtime" / "vm" / "dart.cc"
    elf_source = sdk_root / "runtime" / "vm" / "elf.cc"
    make_version = sdk_root / "tools" / "make_version.py"
    for path in (
        constants,
        calling,
        thread,
        function_impl,
        code_entry_kind,
        raw_object,
        tags,
        object_header,
        dart_entry,
        pointer_tagging,
        runtime_api,
        platform_globals,
        locations,
        assembler_arm64,
        stack_frame_arm64,
        stack_frame,
        dart_api_impl,
        il_arm64,
        il_header,
        kernel_flowgraph,
        snapshot_header,
        dart_api,
        dart_source,
        elf_source,
        make_version,
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
    _verify_calling_convention_stack_layout(
        calling_text, locations.read_text(), source_name="current Dart SDK"
    )
    _verify_generated_native_transition_contract(
        assembler_arm64.read_text(),
        stack_frame_arm64.read_text(),
        stack_frame.read_text(),
        dart_api_impl.read_text(),
        source_name="current Dart SDK",
    )
    _verify_arm64_return_frame_identity(
        assembler_arm64.read_text(), il_arm64.read_text(), source_name="current Dart SDK"
    )

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
    _verify_closure_stack_contract(
        function_text, dart_entry.read_text(), source_name="current Dart SDK"
    )
    _verify_closure_call_descriptor_contract(
        il_header.read_text(), kernel_flowgraph.read_text(), source_name="current Dart SDK"
    )

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
    for profile in _load_manifest():
        _verify_raw_object_layout(
            profile, pointer_tagging.read_text(), raw_object_text, runtime_api.read_text(),
            platform_globals.read_text(), source_name="current Dart SDK"
        )
        # The current checkout may be newer than every checked-in historical
        # candidate. Verify only that the source still exposes the semantic
        # anchors; exact per-candidate values are checked against each tag in
        # verify_historical_profiles().
    tags_text = tags.read_text()
    if "Dart" not in _extract_v_macro(tags_text, "VM_TAG_LIST"):
        raise ValueError("current Dart SDK no longer exposes the Dart VM tag")

    object_header_text = object_header.read_text()
    for profile in _load_manifest():
        _verify_function_kinds(profile, raw_object_text, object_header_text)
    _verify_aot_payload_contract(
        object_header_text,
        (sdk_root / "runtime" / "vm" / "app_snapshot.cc").read_text(),
        source_name="current Dart SDK",
    )
    caches_closure_entry = "untag()->entry_point_ = function.entry_point();" in object_header_text
    precompiled_closure_entry = (
        "void set_entry_point(uword entry_point) const" in object_header_text
        and "closure.ptr()->untag()->entry_point_ = entry_point;" in
        (sdk_root / "runtime" / "vm" / "app_snapshot.cc").read_text()
    )
    if not caches_closure_entry and not precompiled_closure_entry:
        raise ValueError("Dart Closure no longer exposes a verified entry-point cache contract")
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
    _verify_snapshot_header_contract(
        snapshot_header.read_text(), "current Dart SDK", expected_full_aot_kind=None
    )
    _verify_snapshot_symbol_contract(
        dart_api.read_text(), "current Dart SDK", expected_symbols=None
    )
    _verify_snapshot_feature_contract(dart_source.read_text(), "current Dart SDK")
    _verify_parameter_flags_contract(runtime_api.read_text(), platform_globals.read_text(), "current Dart SDK")
    _verify_dart_aot_elf_contract(elf_source.read_text(), "current Dart SDK")
    _snapshot_hash_inputs(make_version.read_text(), "current Dart SDK")
    print(f"Dart SDK ARM64 source contract verified: {sdk_root}")


def _render_profile(profile: dict[str, object]) -> str:
    machine = profile["machine"]
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
    raw_object = profile["raw_object"]
    arguments_descriptor = profile["arguments_descriptor"]
    function_kind = profile["function_kind"]
    type_arguments = profile["type_arguments"]
    transition = profile["transition"]
    gp_args = ", ".join(str(value) for value in r["dart_gp_args"])
    fpu_args = ", ".join(str(value) for value in r["dart_fpu_args"])
    return f"""    RuntimeProfileRecord{{
        .abi_id = {json.dumps(profile['abi_id'])},
        .abi_identity = {{
            .full = {json.dumps(profile['abi_identity']['full'])},
            .core = {json.dumps(profile['abi_identity']['core'])},
            .call = {json.dumps(profile['abi_identity']['call'])},
            .object = {json.dumps(profile['abi_identity']['object'])},
            .transition = {json.dumps(profile['abi_identity']['transition'])},
            .exception = {json.dumps(profile['abi_identity']['exception'])},
        }},
        .machine = {{
            .architecture = VmArchitecture::kArm64,
            .pointer_size = {machine['pointer_size']}u,
            .product = {str(bool(machine['product'])).lower()},
            .compressed_pointers = {str(bool(machine['compressed_pointers'])).lower()},
        }},
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
        .raw_object = {{
            .heap_object_tag = {raw_object['heap_object_tag']}u,
            .smi_tag = {raw_object['smi_tag']}u,
            .smi_tag_mask = {raw_object['smi_tag_mask']}u,
            .smi_tag_shift = {raw_object['smi_tag_shift']}u,
            .class_id_tag_shift = {raw_object['class_id_tag_shift']}u,
            .class_id_tag_bits = {raw_object['class_id_tag_bits']}u,
            .compressed_word_size = {raw_object['compressed_word_size']}u,
        }},
        .arguments_descriptor = {{
            .type_args_len_offset = {_u(int(arguments_descriptor['type_args_len']))},
            .count_offset = {_u(int(arguments_descriptor['count']))},
            .size_offset = {_u(int(arguments_descriptor['size']))},
            .positional_count_offset = {_u(int(arguments_descriptor['positional_count']))},
            .first_named_entry_offset = {_u(int(arguments_descriptor['first_named_entry']))},
            .named_entry_size = {_u(int(arguments_descriptor['named_entry_size']))},
            .name_offset = {_u(int(arguments_descriptor['name']))},
            .position_offset = {_u(int(arguments_descriptor['position']))},
        }},
        .function_kind = {{
            .regular = {function_kind['regular']}u,
            .closure = {function_kind['closure']}u,
            .implicit_closure = {function_kind['implicit_closure']}u,
            .tag_shift = {function_kind['tag_shift']}u,
            .tag_bits = {function_kind['tag_bits']}u,
        }},
        .thread_bridge = {{
            .enter_safepoint_stub_offset = {_u(int(thread['enter_safepoint_stub']))},
            .exit_safepoint_stub_offset = {_u(int(thread['exit_safepoint_stub']))},
            .top_exit_frame_offset = {_u(int(thread['top_exit_frame']))},
            .vm_tag_offset = {_u(int(thread['vm_tag']))},
            .active_exception_offset = {_u(int(thread['active_exception']))},
            .active_stacktrace_offset = {_u(int(thread['active_stacktrace']))},
            .execution_state_offset = {_u(int(thread['execution_state']))},
            .exit_through_ffi_offset = {_u(int(thread['exit_through_ffi']))},
        }},
        .type_arguments = {{
            .cid = {type_arguments['cid']}u,
            .length_offset = {_u(int(type_arguments['length']))},
            .types_offset = {_u(int(type_arguments['types']))},
        }},
        .transition = {{
            .vm_tag_dart = {transition['vm_tag_dart']}u,
            .execution_vm = {transition['execution_vm']}u,
            .execution_generated = {transition['execution_generated']}u,
            .execution_native = {transition['execution_native']}u,
            .exit_none = {transition['exit_none']}u,
            .exit_through_ffi = {transition['exit_through_ffi']}u,
            .exit_through_runtime_call = {transition['exit_through_runtime_call']}u,
        }},
    }}"""


def _fingerprint_function_name(capability: str) -> str:
    return "".join(part.capitalize() for part in capability.split("_"))


def render_capability_fingerprints() -> str:
    functions: list[str] = []
    compositions = {
        "invocation_call_abi": ("function_code_layout",),
        "aot_entry_layout": ("function_code_layout",),
    }
    emitted: set[str] = set()

    def emit(capability: str) -> None:
        if capability in emitted:
            return
        dependencies = compositions.get(capability, ())
        for dependency in dependencies:
            emit(dependency)
        inherited = {
            field
            for dependency in dependencies
            for field in CAPABILITY_FINGERPRINT_FIELDS[dependency]
        }
        fields = [field for field in CAPABILITY_FINGERPRINT_FIELDS[capability] if field not in inherited]
        body_lines = [
            f"    Append{_fingerprint_function_name(dependency)}Fields(key, profile);"
            for dependency in dependencies
        ]
        body_lines.extend(
            f"    AppendCapabilityValue(key, {expression});  // {path}"
            for path, expression in fields
        )
        body = "\n".join(body_lines)
        functions.append(
            f"inline void Append{_fingerprint_function_name(capability)}Fields("
            "std::string& key, const RuntimeProfileRecord& profile) {\n"
            f"{body}\n"
            "}"
        )
        emitted.add(capability)

    for capability in CAPABILITY_FINGERPRINT_FIELDS:
        emit(capability)
    return f"""// Generated by scripts/generate_vm_profiles.py. Do not edit.
#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "vm/runtime_profiles.h"

namespace dartplant::vm_abi::generated {{

template <typename T>
inline void AppendCapabilityValue(std::string& key, const T& value) {{
    key.append(reinterpret_cast<const char*>(&value), sizeof(value));
}}

template <typename T, size_t N>
inline void AppendCapabilityValue(std::string& key, const std::array<T, N>& values) {{
    for (const T& value : values) AppendCapabilityValue(key, value);
}}

{chr(10).join(functions)}

}}  // namespace dartplant::vm_abi::generated
"""


def _load_capability_registry(path: Path = CAPABILITY_MANIFEST) -> list[dict[str, object]]:
    document = json.loads(path.read_text())
    if document.get("schema_version") != 1:
        raise ValueError("unsupported VM capability registry schema")
    capabilities = document.get("capabilities")
    if not isinstance(capabilities, list) or not capabilities:
        raise ValueError("VM capability registry is empty")
    keys: set[str] = set()
    names: set[str] = set()
    bits: set[int] = set()
    symbols: set[str] = set()
    for capability in capabilities:
        if not isinstance(capability, dict):
            raise ValueError("VM capability registry entry must be an object")
        key = capability.get("key")
        name = capability.get("diagnostic_name")
        symbol = capability.get("cpp_symbol")
        bit = capability.get("bit")
        if not isinstance(key, str) or not key or key in keys:
            raise ValueError(f"invalid/duplicate VM capability key: {key!r}")
        if not isinstance(name, str) or not name or name in names:
            raise ValueError(f"invalid/duplicate VM capability diagnostic name: {name!r}")
        if not isinstance(symbol, str) or not symbol or symbol in symbols:
            raise ValueError(f"invalid/duplicate VM capability C++ symbol: {symbol!r}")
        if not isinstance(bit, int) or bit <= 0 or bit & (bit - 1) or bit in bits:
            raise ValueError(f"invalid/duplicate VM capability bit: {bit!r}")
        for flag in ("cold_required", "verified_after_create", "ci_required_event"):
            if not isinstance(capability.get(flag), bool):
                raise ValueError(f"{key}: VM capability registry field {flag} must be boolean")
        keys.add(key)
        names.add(name)
        symbols.add(symbol)
        bits.add(bit)
    expected_keys = set(CAPABILITY_FINGERPRINT_FIELDS) | {
        "artifact_lifecycle",
        "closure_call_layout",
    }
    if keys != expected_keys:
        missing = sorted(expected_keys - keys)
        extra = sorted(keys - expected_keys)
        raise ValueError(f"VM capability registry/fingerprint mismatch missing={missing} extra={extra}")
    return capabilities


def render_capability_registry() -> str:
    capabilities = _load_capability_registry()
    entries = []
    assertions = []
    required_symbols = []
    verified_symbols = []
    for capability in capabilities:
        symbol = str(capability["cpp_symbol"])
        bit = int(capability["bit"])
        entries.append(
            "    {"
            f"{symbol}, \"{capability['key']}\", \"{capability['diagnostic_name']}\", "
            f"{str(capability['cold_required']).lower()}, "
            f"{str(capability['verified_after_create']).lower()}, "
            f"{str(capability['ci_required_event']).lower()}"
            "},"
        )
        assertions.append(
            f"static_assert(static_cast<uint64_t>({symbol}) == UINT64_C({bit}));"
        )
        if capability["cold_required"]:
            required_symbols.append(symbol)
        if capability["verified_after_create"]:
            verified_symbols.append(symbol)

    def mask_expression(symbols: list[str]) -> str:
        return " |\n    ".join(symbols) if symbols else "kCapabilityNone"

    return f"""// Generated by scripts/generate_vm_profiles.py. Do not edit.
#pragma once

#include <cstdint>

namespace dartplant::vm_abi::generated {{

inline constexpr CapabilityDescriptor kCapabilityRegistry[] = {{
{chr(10).join(entries)}
}};

inline constexpr uint64_t kColdRequiredCapabilityMask =
    {mask_expression(required_symbols)};
inline constexpr uint64_t kVerifiedAfterCreateCapabilityMask =
    {mask_expression(verified_symbols)};

{chr(10).join(assertions)}

}}  // namespace dartplant::vm_abi::generated
"""


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
    expected_fingerprints = render_capability_fingerprints()
    expected_capabilities = render_capability_registry()
    if sdk_root is not None:
        verify_sdk_contract(sdk_root)
        verify_historical_profiles(sdk_root, profiles)
    if check:
        if (
            not GENERATED.is_file()
            or GENERATED.read_text() != expected
            or not GENERATED_FINGERPRINTS.is_file()
            or GENERATED_FINGERPRINTS.read_text() != expected_fingerprints
            or not GENERATED_CAPABILITIES.is_file()
            or GENERATED_CAPABILITIES.read_text() != expected_capabilities
        ):
            raise RuntimeError(
                "generated Dart VM profiles are stale; run "
                "python3 scripts/main.py profiles"
            )
        print(
            "VM profiles and capability fingerprints verified: "
            f"{GENERATED.relative_to(ROOT_DIR)}, "
            f"{GENERATED_FINGERPRINTS.relative_to(ROOT_DIR)}, "
            f"{GENERATED_CAPABILITIES.relative_to(ROOT_DIR)}"
        )
        return
    GENERATED.parent.mkdir(parents=True, exist_ok=True)
    GENERATED.write_text(expected)
    GENERATED_FINGERPRINTS.write_text(expected_fingerprints)
    GENERATED_CAPABILITIES.write_text(expected_capabilities)
    print(
        "VM profiles and capability fingerprints generated: "
        f"{GENERATED.relative_to(ROOT_DIR)}, "
        f"{GENERATED_FINGERPRINTS.relative_to(ROOT_DIR)}, "
        f"{GENERATED_CAPABILITIES.relative_to(ROOT_DIR)}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--sdk-root", type=Path)
    args = parser.parse_args()
    run(check=args.check, sdk_root=args.sdk_root)


if __name__ == "__main__":
    main()
