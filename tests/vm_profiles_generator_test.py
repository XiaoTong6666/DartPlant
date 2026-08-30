from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import generate_vm_profiles  # noqa: E402


class VmProfilesGeneratorTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
