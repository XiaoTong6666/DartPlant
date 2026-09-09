from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import generate_vm_profiles  # noqa: E402


class VmProfilesGeneratorTest(unittest.TestCase):
    def test_domain_abi_identity_is_exhaustive_deterministic_and_sensitive(self) -> None:
        profile = copy.deepcopy(generate_vm_profiles._load_manifest()[0])
        generate_vm_profiles._verify_abi_domain_coverage(profile)
        original = {
            domain: generate_vm_profiles._canonical_abi_id(profile, domain)
            for domain in ("full", *generate_vm_profiles.ABI_DOMAIN_FIELDS)
        }
        reordered = dict(reversed(list(profile.items())))
        self.assertEqual(
            original,
            {
                domain: generate_vm_profiles._canonical_abi_id(reordered, domain)
                for domain in ("full", *generate_vm_profiles.ABI_DOMAIN_FIELDS)
            },
        )

        profile["function_type"]["parameter_types"] += 4
        mutated = {
            domain: generate_vm_profiles._canonical_abi_id(profile, domain)
            for domain in ("full", *generate_vm_profiles.ABI_DOMAIN_FIELDS)
        }
        self.assertNotEqual(original["full"], mutated["full"])
        self.assertNotEqual(original["object"], mutated["object"])
        self.assertEqual(original["core"], mutated["core"])
        self.assertEqual(original["transition"], mutated["transition"])
        self.assertEqual(original["call"], mutated["call"])
        self.assertEqual(original["exception"], mutated["exception"])

    def test_manifest_rejects_stale_domain_identity(self) -> None:
        manifest = json.loads(generate_vm_profiles.MANIFEST.read_text())
        manifest["profiles"][0]["abi_identity"]["object"] = "stale"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profiles.json"
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "abi_identity is stale"):
                generate_vm_profiles._load_manifest(path)

    def test_selects_aot_product_arm64_compressed_block(self) -> None:
        text = r"""
#if defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \
    defined(DART_COMPRESSED_POINTERS)
static constexpr dart::compiler::target::word Function_code_offset = 0x44;
#endif
#if defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \
    defined(DART_COMPRESSED_POINTERS)
static constexpr dart::compiler::target::word AOT_Function_code_offset = 0x2c;
static constexpr dart::compiler::target::word AOT_Function_entry_point_offset[] = {0x8, 0x10};
#endif
"""
        block = generate_vm_profiles._product_arm64_compressed_aot_block(text)
        self.assertIn("AOT_Function_code_offset", block)
        self.assertEqual(
            0x2C,
            generate_vm_profiles._parse_aot_offset(block, "AOT_Function_code_offset"),
        )
        self.assertEqual(
            0x8,
            generate_vm_profiles._parse_aot_offset(block, "AOT_Function_entry_point_offset"),
        )
        self.assertEqual(
            0x10,
            generate_vm_profiles._parse_aot_offset_at(
                block, "AOT_Function_entry_point_offset", 1
            ),
        )

    def test_selects_aot_nonproduct_arm64_compressed_block(self) -> None:
        text = r"""
#if defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \
    defined(DART_COMPRESSED_POINTERS)
static constexpr dart::compiler::target::word AOT_Function_code_offset = 0x2c;
static constexpr dart::compiler::target::word AOT_Function_kind_tag_offset = 0x30;
#endif
#if !defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \
    defined(DART_COMPRESSED_POINTERS)
static constexpr dart::compiler::target::word AOT_Function_code_offset = 0x2c;
static constexpr dart::compiler::target::word AOT_Function_kind_tag_offset = 0x34;
#endif
"""
        block = generate_vm_profiles._nonproduct_arm64_compressed_aot_block(text)
        self.assertEqual(
            0x34,
            generate_vm_profiles._parse_aot_offset(block, "AOT_Function_kind_tag_offset"),
        )

    def test_manifest_allows_same_snapshot_hash_for_distinct_build_profiles(self) -> None:
        profiles = generate_vm_profiles._load_manifest()
        product = next(
            profile
            for profile in profiles
            if profile["dart_version"] == "3.4.4" and profile["machine"]["product"]
        )
        nonproduct = next(
            profile
            for profile in profiles
            if profile["dart_version"] == "3.4.4" and not profile["machine"]["product"]
        )
        self.assertEqual(product["snapshot_hash"], nonproduct["snapshot_hash"])
        self.assertNotEqual(product["snapshot_profile"], nonproduct["snapshot_profile"])
        self.assertNotEqual(product["abi_id"], nonproduct["abi_id"])
        self.assertEqual(0x30, product["function"]["kind_tag"])
        self.assertEqual(0x34, nonproduct["function"]["kind_tag"])
        self.assertEqual(0x10, product["class_table"]["num_cids"])
        self.assertEqual(0x18, nonproduct["class_table"]["num_cids"])
        self.assertEqual(0x0C, product["class"]["functions"])
        self.assertEqual(0x10, nonproduct["class"]["functions"])
        self.assertEqual(0x24, product["class"]["library"])
        self.assertEqual(0x28, nonproduct["class"]["library"])

    def test_nonproduct_class_table_num_cids_is_source_proven(self) -> None:
        profile = copy.deepcopy(
            next(
                profile
                for profile in generate_vm_profiles._load_manifest()
                if profile["dart_version"] == "3.4.4"
                and not profile["machine"]["product"]
            )
        )
        runtime_offsets = r"""
#if !defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \
    defined(DART_COMPRESSED_POINTERS)
static constexpr dart::compiler::target::word AOT_Function_code_offset = 0x2c;
static constexpr dart::compiler::target::word
    AOT_ClassTable_allocation_tracing_state_table_offset = 0x8;
#endif
"""
        class_table_header = """
template <typename CidType, typename... Columns>
class CidIndexedTable {
 private:
  ClassTableAllocator* allocator_;
  intptr_t num_cids_ = 0;
  intptr_t capacity_ = 0;
};
class ClassTable {
 public:
  static intptr_t allocation_tracing_state_table_offset() {
    static_assert(sizeof(cached_allocation_tracing_state_table_) == kWordSize);
    return OFFSET_OF(ClassTable, cached_allocation_tracing_state_table_);
  }
 private:
  ClassTableAllocator* allocator_;
  NOT_IN_PRODUCT(AcqRelAtomic<uint8_t*> cached_allocation_tracing_state_table_ =
                     {nullptr});
};
"""
        generate_vm_profiles._verify_class_table_num_cids_contract(
            profile,
            runtime_offsets,
            class_table_header,
            source_name="test",
        )

        profile["class_table"]["num_cids"] = 0x10
        with self.assertRaisesRegex(ValueError, "class_table.num_cids"):
            generate_vm_profiles._verify_class_table_num_cids_contract(
                profile,
                runtime_offsets,
                class_table_header,
                source_name="test",
            )

    def test_nonproduct_class_raw_layout_is_source_proven(self) -> None:
        profile = copy.deepcopy(
            next(
                profile
                for profile in generate_vm_profiles._load_manifest()
                if profile["dart_version"] == "3.4.4"
                and not profile["machine"]["product"]
            )
        )
        raw_object = """
class UntaggedClass : public UntaggedObject {
  COMPRESSED_POINTER_FIELD(StringPtr, name)
  VISIT_FROM(name)
  NOT_IN_PRODUCT(COMPRESSED_POINTER_FIELD(StringPtr, user_name))
  COMPRESSED_POINTER_FIELD(ArrayPtr, functions)
  COMPRESSED_POINTER_FIELD(ArrayPtr, functions_hash_table)
  COMPRESSED_POINTER_FIELD(ArrayPtr, fields)
  COMPRESSED_POINTER_FIELD(ArrayPtr, offset_in_words_to_field)
  COMPRESSED_POINTER_FIELD(ArrayPtr, interfaces)
  COMPRESSED_POINTER_FIELD(ScriptPtr, script)
  COMPRESSED_POINTER_FIELD(LibraryPtr, library)
};
"""
        generate_vm_profiles._verify_class_raw_layout_contract(
            profile, raw_object, source_name="test"
        )

        profile["class"]["functions"] = 0x0C
        with self.assertRaisesRegex(ValueError, "class.functions"):
            generate_vm_profiles._verify_class_raw_layout_contract(
                profile, raw_object, source_name="test"
            )

    def test_nonproduct_code_instructions_length_is_source_proven(self) -> None:
        profile = copy.deepcopy(
            next(
                profile
                for profile in generate_vm_profiles._load_manifest()
                if profile["dart_version"] == "3.4.4"
                and not profile["machine"]["product"]
            )
        )
        raw_object = """
class UntaggedCode : public UntaggedObject {
  POINTER_FIELD(ObjectPtr, owner)
  POINTER_FIELD(ExceptionHandlersPtr, exception_handlers)
  POINTER_FIELD(PcDescriptorsPtr, pc_descriptors)
  POINTER_FIELD(ObjectPtr, catch_entry)
  POINTER_FIELD(CompressedStackMapsPtr, compressed_stackmaps)
  POINTER_FIELD(ArrayPtr, inlined_id_to_function)
  POINTER_FIELD(CodeSourceMapPtr, code_source_map)
  NOT_IN_PRODUCT(POINTER_FIELD(ObjectPtr, return_address_metadata))
  NOT_IN_PRODUCT(POINTER_FIELD(LocalVarDescriptorsPtr, var_descriptors))
  NOT_IN_PRODUCT(POINTER_FIELD(ArrayPtr, comments))
  NOT_IN_PRODUCT(alignas(8) int64_t compile_timestamp_);
  int32_t state_bits_;
  ONLY_IN_PRECOMPILED(uint32_t instructions_length_);
};
class UntaggedObjectPool : public UntaggedObject {};
"""
        profile["code"]["instructions_length"] = 0x94
        generate_vm_profiles._verify_code_instructions_length_contract(
            profile, raw_object, source_name="test"
        )

        profile["code"]["instructions_length"] = 0x74
        with self.assertRaisesRegex(ValueError, "code.instructions_length"):
            generate_vm_profiles._verify_code_instructions_length_contract(
                profile, raw_object, source_name="test"
            )

    def test_profile_verifier_rejects_source_offset_drift(self) -> None:
        profile = generate_vm_profiles._load_manifest()[0]
        bindings = generate_vm_profiles.AOT_OFFSET_BINDINGS
        lines = [
            "#if defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \\",
            "    defined(DART_COMPRESSED_POINTERS)",
        ]
        emitted_arrays: set[str] = set()
        for (section, field), sdk_name in bindings.items():
            array_binding = generate_vm_profiles.AOT_ARRAY_OFFSET_BINDINGS.get(
                (section, field)
            )
            if array_binding is not None:
                array_name = array_binding[0]
                if array_name in emitted_arrays:
                    continue
                emitted_arrays.add(array_name)
                if array_name == "AOT_Code_entry_point_offset":
                    code = profile["code"]
                    values = [
                        code["entry_point"],
                        code["unchecked_entry_point"],
                        code["monomorphic_entry_point"],
                        code["monomorphic_unchecked_entry_point"],
                    ]
                else:
                    function = profile["function"]
                    values = [
                        function["entry_point"],
                        function["unchecked_entry_point"],
                    ]
                rendered = ", ".join(f"0x{int(value):x}" for value in values)
                lines.append(
                    "static constexpr dart::compiler::target::word "
                    f"{array_name}[] = {{{rendered}}};"
                )
                continue
            value = int(profile[section][field])
            lines.append(
                "static constexpr dart::compiler::target::word "
                f"{sdk_name} = 0x{value:x};"
            )
        lines.append("#endif")
        text = "\n".join(lines)
        generate_vm_profiles._verify_profile_against_aot_offsets(profile, text)

        drifted = text.replace(
            "AOT_Thread_heap_base_offset = 0x48;",
            "AOT_Thread_heap_base_offset = 0x50;",
        )
        with self.assertRaisesRegex(ValueError, "thread.heap_base"):
            generate_vm_profiles._verify_profile_against_aot_offsets(profile, drifted)

        descriptor_drift = text.replace(
            "AOT_ArgumentsDescriptor_count_offset = 0x14;",
            "AOT_ArgumentsDescriptor_count_offset = 0x18;",
        )
        with self.assertRaisesRegex(ValueError, "arguments_descriptor.count"):
            generate_vm_profiles._verify_profile_against_aot_offsets(
                profile, descriptor_drift
            )

    def test_payload_contract_verifier_requires_dart_payload_start_semantics(self) -> None:
        object_header = """
bool HasMonomorphicEntry(const CodePtr code) {
  return code->untag()->entry_point_ != code->untag()->monomorphic_entry_point_;
}
uword PayloadStartOf(const CodePtr code) {
  const uword entry_offset = HasMonomorphicEntry(code)
      ? Instructions::kPolymorphicEntryOffsetAOT
      : 0;
  return EntryPointOf(code) - entry_offset;
}
"""
        app_snapshot = """
uword start = Code::PayloadStartOf(code);
code->untag()->instructions_length_ = previous_end - start;
"""
        generate_vm_profiles._verify_aot_payload_contract(
            object_header, app_snapshot, source_name="test"
        )
        with self.assertRaisesRegex(ValueError, "PayloadStartOf"):
            generate_vm_profiles._verify_aot_payload_contract(
                object_header.replace("entry_point_ !=", "entry_point_ =="),
                app_snapshot,
                source_name="test",
            )

    def test_function_kind_verifier_rejects_closure_numbering_drift(self) -> None:
        profile = generate_vm_profiles._load_manifest()[0]
        source = r"""
#define FOR_EACH_RAW_FUNCTION_KIND(V) \
  V(RegularFunction) \
  V(ClosureFunction) \
  V(ImplicitClosureFunction) \
  V(GetterFunction)
"""
        object_header = """
using KindBits = BitField<decltype(UntaggedFunction::kind_tag_),
                          UntaggedFunction::Kind,
                          0,
                          UntaggedFunction::kKindBitSize>;
"""
        # The synthetic source has four kinds and therefore needs two bits.
        profile = copy.deepcopy(profile)
        profile["function_kind"]["tag_bits"] = 2
        generate_vm_profiles._verify_function_kinds(profile, source, object_header)
        drifted = source.replace(
            "V(ClosureFunction) \\",
            "V(GetterFunction) \\\n+  V(ClosureFunction) \\",
        )
        with self.assertRaisesRegex(ValueError, "ClosureFunction"):
            generate_vm_profiles._verify_function_kinds(profile, drifted, object_header)

    def test_closure_stack_verifier_requires_forced_stack_and_boxed_descriptor(self) -> None:
        function_impl = """
intptr_t Function::MaxNumberOfParametersInRegisters(Zone* zone) const {
  switch (kind()) {
    case UntaggedFunction::kClosureFunction:
    case UntaggedFunction::kImplicitClosureFunction:
      return 0;
    default:
      return num_fixed_parameters();
  }
}
"""
        dart_entry = """
// Right now this is for example the case for all closure functions.
return New(type_args_len, num_arguments, num_arguments,
           optional_arguments_names, space);
"""
        generate_vm_profiles._verify_closure_stack_contract(
            function_impl, dart_entry, "test"
        )
        with self.assertRaisesRegex(ValueError, "force stack"):
            generate_vm_profiles._verify_closure_stack_contract(
                function_impl.replace("return 0;", "return num_fixed_parameters();"),
                dart_entry,
                "test",
            )
        with self.assertRaisesRegex(ValueError, "boxed closure"):
            generate_vm_profiles._verify_closure_stack_contract(
                function_impl,
                dart_entry.replace(
                    "num_arguments, num_arguments", "num_arguments, size_arguments"
                ),
                "test",
            )

    def test_closure_descriptor_verifier_includes_hidden_receiver(self) -> None:
        il_header = """
template <intptr_t kExtraInputs>
class TemplateDartCall : public VariadicDefinition {
 public:
  intptr_t FirstArgIndex() const { return type_args_len_ > 0 ? 1 : 0; }
  intptr_t ArgumentCount() const {
    return move_arguments_ != nullptr ? move_arguments_->length()
                                      : InputCount() - kExtraInputs;
  }
  ArrayPtr GetArgumentsDescriptor() const {
    return ArgumentsDescriptor::New(
        type_args_len(), ArgumentCountWithoutTypeArgs(),
        ArgumentsSizeWithoutTypeArgs(), argument_names());
  }
};
class ClosureCallInstr : public TemplateDartCall<1> {};
"""
        kernel_flowgraph = """
instructions += BuildArguments(&argument_names, &argument_count,
                               &positional_argument_count);
++argument_count;  // include receiver
instructions += B->ClosureCall(target_function, position, type_args_len,
                               argument_count, argument_names, &result_type);
"""
        generate_vm_profiles._verify_closure_call_descriptor_contract(
            il_header, kernel_flowgraph, "test"
        )
        with self.assertRaisesRegex(ValueError, "input accounting"):
            generate_vm_profiles._verify_closure_call_descriptor_contract(
                il_header.replace(
                    "class ClosureCallInstr : public TemplateDartCall<1>",
                    "class ClosureCallInstr : public TemplateDartCall<0>",
                ),
                kernel_flowgraph,
                "test",
            )
        with self.assertRaisesRegex(ValueError, "hidden-receiver accounting"):
            generate_vm_profiles._verify_closure_call_descriptor_contract(
                il_header,
                kernel_flowgraph.replace("++argument_count", "argument_count += 0"),
                "test",
            )

    def test_raw_object_layout_verifier_supports_old_and_new_dart_spellings(self) -> None:
        profile = generate_vm_profiles._load_manifest()[0]
        pointer_tagging = """
enum {
  kSmiTag = 0,
  kHeapObjectTag = 1,
  kSmiTagMask = 1,
  kSmiTagShift = 1,
};
"""
        runtime_api = """
#if defined(DART_COMPRESSED_POINTERS)
static constexpr intptr_t kCompressedWordSize = kInt32Size;
#else
static constexpr intptr_t kCompressedWordSize = kWordSize;
#endif
"""
        platform_globals = "constexpr intptr_t kInt32SizeLog2 = 2;"
        modern_raw = """
using SizeTagBits = BitField<decltype(tags_), intptr_t, kBitsPerInt8, 4>;
using ClassIdTag =
    BitField<decltype(tags_), ClassIdTagType, SizeTagBits::kNextBit, 20>;
"""
        old_raw = """
enum {
  kSizeTagPos = kReservedBit + 1,  // = 8
  kSizeTagSize = 4,
  kClassIdTagPos = kSizeTagPos + kSizeTagSize,  // = 12
  kClassIdTagSize = 20,
};
"""
        for raw in (modern_raw, old_raw):
            generate_vm_profiles._verify_raw_object_layout(
                profile,
                pointer_tagging,
                raw,
                runtime_api,
                platform_globals,
                "test",
            )
        drifted = copy.deepcopy(profile)
        drifted["raw_object"]["class_id_tag_shift"] = 13
        with self.assertRaisesRegex(ValueError, "ClassIdTag"):
            generate_vm_profiles._verify_raw_object_layout(
                drifted,
                pointer_tagging,
                modern_raw,
                runtime_api,
                platform_globals,
                "test",
            )

    def test_stack_layout_verifier_requires_reverse_entry_sp_mapping(self) -> None:
        calling = """
if (i < max_arguments_in_registers) {}
const intptr_t offset_to_last_parameter_slot_from_fp =
    (compiler::target::frame_layout.param_end_from_fp + 1);
intptr_t offset_in_words_from_fp = offset_to_last_parameter_slot_from_fp;
for (intptr_t i = argc - 1; i >= 0; --i) {}
"""
        locations = """
const auto fp_to_entry_sp_delta =
    (compiler::target::frame_layout.param_end_from_fp + 1) -
    compiler::target::frame_layout.last_param_from_entry_sp;
return ToSpRelative(fp_to_entry_sp_delta);
"""
        generate_vm_profiles._verify_calling_convention_stack_layout(
            calling, locations, "test"
        )
        with self.assertRaisesRegex(ValueError, "entry-SP"):
            generate_vm_profiles._verify_calling_convention_stack_layout(
                calling,
                locations.replace("last_param_from_entry_sp", "saved_caller_fp_from_fp"),
                "test",
            )

    def test_generated_native_verifier_requires_exit_frame_and_full_safepoint(self) -> None:
        assembler = """
void Assembler::TransitionGeneratedToNative(Register destination,
                                            Register new_exit_frame,
                                            Register new_exit_through_ffi,
                                            bool enter_safepoint) {
  StoreToOffset(new_exit_frame, THR, target::Thread::top_exit_frame_info_offset());
  StoreToOffset(new_exit_through_ffi, THR, target::Thread::exit_through_ffi_offset());
  Register tmp = new_exit_through_ffi;
  StoreToOffset(destination, THR, target::Thread::vm_tag_offset());
  LoadImmediate(tmp, target::Thread::native_execution_state());
  StoreToOffset(tmp, THR, target::Thread::execution_state_offset());
  EnterFullSafepoint(tmp);
}
void Assembler::TransitionNativeToGenerated(Register state, bool exit_safepoint) {
  ExitFullSafepoint(state);
  LoadImmediate(state, target::Thread::generated_execution_state());
  StoreToOffset(state, THR, target::Thread::execution_state_offset());
  StoreToOffset(ZR, THR, target::Thread::top_exit_frame_info_offset());
  StoreToOffset(state, THR, target::Thread::exit_through_ffi_offset());
}
"""
        frame_layout = """
static constexpr int kFirstObjectSlotFromFp = -1;
static constexpr int kLastFixedObjectSlotFromFp = -2;
static constexpr int kSavedCallerFpSlotFromFp = 0;
static constexpr int kSavedCallerPcSlotFromFp = 1;
static constexpr int kCallerSpSlotFromFp = 2;
"""
        stack_frame = """
uword exit_marker = thread_->top_exit_frame_info();
frames_.fp_ = exit_marker;
"""
        dart_api = """
DART_EXPORT void Dart_EnterScope() {
  Thread* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  thread->EnterApiScope();
}
"""
        generate_vm_profiles._verify_generated_native_transition_contract(
            assembler, frame_layout, stack_frame, dart_api, "test"
        )
        with self.assertRaisesRegex(ValueError, "Generated->Native"):
            generate_vm_profiles._verify_generated_native_transition_contract(
                assembler.replace("EnterFullSafepoint(tmp);", ""),
                frame_layout,
                stack_frame,
                dart_api,
                "test",
            )
        with self.assertRaisesRegex(ValueError, "synthetic ExitFrame"):
            generate_vm_profiles._verify_generated_native_transition_contract(
                assembler,
                frame_layout.replace(
                    "kLastFixedObjectSlotFromFp = -2;",
                    "kLastFixedObjectSlotFromFp = -3;",
                ),
                stack_frame,
                dart_api,
                "test",
            )

    def test_arm64_return_frame_verifier_requires_exact_caller_identity_restore(self) -> None:
        assembler = """
void Assembler::EnterFrame(intptr_t frame_size) {
  SPILLS_LR_TO_FRAME(PushPair(FP, LR));
  mov(FP, SP);
}
void Assembler::LeaveFrame() {
  mov(SP, FP);
  RESTORES_LR_FROM_FRAME(PopPair(FP, LR));
}
void Assembler::EnterDartFrame(intptr_t frame_size, Register new_pp) {
  EnterFrame(0);
}
void Assembler::LeaveDartFrame() {
  LeaveFrame();
}
"""
        il_arm64 = """
void DartReturnInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (!compiler->flow_graph().graph_entry()->NeedsFrame()) {
    __ ret();
    return;
  }
  __ LeaveDartFrame();
  __ ret();
}
"""
        generate_vm_profiles._verify_arm64_return_frame_identity(
            assembler, il_arm64, "test"
        )
        with self.assertRaisesRegex(ValueError, "entry SPREG"):
            generate_vm_profiles._verify_arm64_return_frame_identity(
                assembler.replace("mov(SP, FP);", "mov(SP, R20);"),
                il_arm64,
                "test",
            )
        with self.assertRaisesRegex(ValueError, "caller FP/LR"):
            generate_vm_profiles._verify_arm64_return_frame_identity(
                assembler.replace(
                    "RESTORES_LR_FROM_FRAME(PopPair(FP, LR));",
                    "RESTORES_LR_FROM_FRAME(PopPair(R20, LR));",
                ),
                il_arm64,
                "test",
            )
        with self.assertRaisesRegex(ValueError, "framed Dart return"):
            generate_vm_profiles._verify_arm64_return_frame_identity(
                assembler,
                il_arm64.replace("__ LeaveDartFrame();", "__ LeaveFrame();"),
                "test",
            )

    def test_cid_verifier_rejects_manifest_drift_from_source_order(self) -> None:
        source = r"""
#define CLASS_LIST(V) \
  V(Object) \
  V(Class) \
  V(Function) \
  V(Library) \
  V(Code) \
  V(ObjectPool) \
  V(Instance) \
  V(AbstractType) \
  V(Type) \
  V(TypeArguments) \
  V(FunctionType) \
  V(RecordType) \
  V(TypeParameter) \
  V(Bool) \
  V(Array) \
  V(ImmutableArray) \
  V(GrowableObjectArray) \
  V(String) \
  V(OneByteString) \
  V(TwoByteString)
#define CLASS_LIST_FFI(V)
#define CLASS_LIST_TYPED_DATA(V)
"""
        actual = generate_vm_profiles._class_id_map(source)
        profile = copy.deepcopy(generate_vm_profiles._load_manifest()[0])
        cid_fields = {
            "class": "ClassCid",
            "function": "FunctionCid",
            "library": "LibraryCid",
            "code": "CodeCid",
            "object_pool": "ObjectPoolCid",
            "array": "ArrayCid",
            "immutable_array": "ImmutableArrayCid",
            "growable_object_array": "GrowableObjectArrayCid",
            "one_byte_string": "OneByteStringCid",
            "two_byte_string": "TwoByteStringCid",
        }
        for field, name in cid_fields.items():
            profile["cids"][field] = actual[name]
        profile["type_arguments"]["cid"] = actual["TypeArgumentsCid"]
        profile["canonical_bool"]["cid"] = actual["BoolCid"]
        type_fields = {
            "cid_type": "TypeCid",
            "cid_function_type": "FunctionTypeCid",
            "cid_record_type": "RecordTypeCid",
            "cid_type_parameter": "TypeParameterCid",
            "cid_null": "NullCid",
            "cid_dynamic": "DynamicCid",
            "cid_void": "VoidCid",
            "cid_never": "NeverCid",
        }
        for field, name in type_fields.items():
            profile["function_type"][field] = actual[name]
        generate_vm_profiles._verify_class_ids(profile, source)
        profile["cids"]["function"] += 1
        with self.assertRaisesRegex(ValueError, "FunctionCid"):
            generate_vm_profiles._verify_class_ids(profile, source)

    def test_abi_layer_separation_rejects_provenance_and_artifact_facts(self) -> None:
        profile = copy.deepcopy(generate_vm_profiles._load_manifest()[0])
        domains = dict(generate_vm_profiles.ABI_DOMAIN_FIELDS)
        domains["core"] = (*domains["core"], "snapshot_hash")
        with self.assertRaisesRegex(ValueError, "provenance-only"):
            generate_vm_profiles._verify_abi_layer_separation(
                profile, domain_fields=domains
            )

        capabilities = dict(generate_vm_profiles.CAPABILITY_FINGERPRINT_FIELDS)
        capabilities["runtime_roots"] = (
            *capabilities["runtime_roots"],
            ("code.entry_va", "forbidden"),
        )
        with self.assertRaisesRegex(ValueError, "artifact/runtime"):
            generate_vm_profiles._verify_abi_layer_separation(
                profile, capability_fields=capabilities
            )

        polluted = copy.deepcopy(profile)
        polluted["function"]["build_id"] = 1
        with self.assertRaisesRegex(ValueError, "RuntimeProfileRecord ABI"):
            generate_vm_profiles._verify_abi_layer_separation(polluted)

    def test_snapshot_header_source_contract_checks_full_layout_and_kind(self) -> None:
        source = """
class Snapshot {
 public:
  enum Kind { kFull, kFullCore, kFullJIT, kFullAOT, kNone, kInvalid };
  static constexpr int32_t kMagicValue = 0xdcdcf5f5;
  static constexpr intptr_t kMagicOffset = 0;
  static constexpr intptr_t kMagicSize = sizeof(int32_t);
  static constexpr intptr_t kLengthOffset = kMagicOffset + kMagicSize;
  static constexpr intptr_t kLengthSize = sizeof(int64_t);
  static constexpr intptr_t kKindOffset = kLengthOffset + kLengthSize;
  static constexpr intptr_t kKindSize = sizeof(int64_t);
  static constexpr intptr_t kHeaderSize = kKindOffset + kKindSize;
  int64_t large_length() const {
    return Read<int64_t>(kLengthOffset) + kMagicSize;
  }
  void set_length(intptr_t value) {
    return Write<int64_t>(kLengthOffset, value - kMagicSize);
  }
};
"""
        generate_vm_profiles._verify_snapshot_header_contract(source, "test")
        with self.assertRaisesRegex(ValueError, "kFullAOT"):
            generate_vm_profiles._verify_snapshot_header_contract(
                source.replace("kFullJIT, kFullAOT", "kFullAOT, kFullJIT"), "test"
            )
        with self.assertRaisesRegex(ValueError, "kLengthOffset"):
            generate_vm_profiles._verify_snapshot_header_contract(
                source.replace(
                    "kLengthOffset = kMagicOffset + kMagicSize",
                    "kLengthOffset = 8",
                ),
                "test",
            )

    def test_snapshot_symbol_and_named_parameter_contracts_are_source_proven(self) -> None:
        dart_api = "\n".join(
            f'#define {macro} "{value}"'
            for macro, value in generate_vm_profiles.EXPECTED_SNAPSHOT_SYMBOLS.items()
        )
        generate_vm_profiles._verify_snapshot_symbol_contract(dart_api, "test")
        with self.assertRaisesRegex(ValueError, "kIsolateSnapshotInstructionsAsmSymbol"):
            generate_vm_profiles._verify_snapshot_symbol_contract(
                dart_api.replace("_kDartIsolateSnapshotInstructions", "_changed"),
                "test",
            )

        runtime_api = """
enum ParameterFlags {
  kRequiredNamedParameterFlag,
  kNumParameterFlags,
};
static constexpr intptr_t kNumParameterFlagsPerElementLog2 =
    kBitsPerWordLog2 - 1 - kNumParameterFlags;
static constexpr intptr_t kNumParameterFlagsPerElement =
    1 << kNumParameterFlagsPerElementLog2;
"""
        globals_source = """
constexpr intptr_t kInt64SizeLog2 = 3;
constexpr intptr_t kBitsPerByteLog2 = 3;
constexpr intptr_t kWordSizeLog2 = kInt64SizeLog2;
constexpr intptr_t kBitsPerWordLog2 = kWordSizeLog2 + kBitsPerByteLog2;
"""
        generate_vm_profiles._verify_parameter_flags_contract(
            runtime_api, globals_source, "test"
        )
        with self.assertRaisesRegex(ValueError, "flag numbering"):
            generate_vm_profiles._verify_parameter_flags_contract(
                runtime_api.replace(
                    "kRequiredNamedParameterFlag,\n  kNumParameterFlags",
                    "kOtherFlag,\n  kRequiredNamedParameterFlag,\n  kNumParameterFlags",
                ),
                globals_source,
                "test",
            )

    def test_manifest_snapshot_hash_recomputes_from_exact_sdk_tag_sources(self) -> None:
        sdk_root = ROOT.parent / "sdk"
        if not (sdk_root / ".git").is_dir():
            self.skipTest("Dart SDK source checkout is unavailable")
        profile = generate_vm_profiles._load_manifest()[0]
        make_version = generate_vm_profiles._git_show(
            sdk_root, str(profile["dart_version"]), "tools/make_version.py"
        )
        generate_vm_profiles._verify_snapshot_source_hash(
            sdk_root,
            str(profile["dart_version"]),
            make_version,
            str(profile["snapshot_hash"]),
            "test Dart SDK",
        )


if __name__ == "__main__":
    unittest.main()
