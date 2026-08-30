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
        class_id = _git_show(sdk_root, version, "runtime/vm/class_id.h")
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
        _verify_profile_against_aot_offsets(profile, runtime_offsets)
        _verify_aot_payload_contract(
            object_header, app_snapshot, source_name=f"Dart SDK {version}"
        )
        _verify_function_kinds(profile, raw_object, object_header)
        _verify_class_ids(profile, class_id)
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
    for path in (
        constants,
        calling,
        thread,
        function_impl,
        code_entry_kind,
        raw_object,
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

    object_header_text = object_header.read_text()
    for profile in _load_manifest():
        _verify_function_kinds(profile, raw_object_text, object_header_text)
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
    raw_object = profile["raw_object"]
    arguments_descriptor = profile["arguments_descriptor"]
    function_kind = profile["function_kind"]
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
