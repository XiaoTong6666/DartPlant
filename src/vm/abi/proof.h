// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_VM_ABI_PROOF_H_
#define DARTPLANT_VM_ABI_PROOF_H_

#include <cstdint>

#include "vm/runtime_profiles.h"

namespace dartplant::vm_abi {

struct RegisterEvidence {
    bool available = false;
    uint64_t pp = 0;
    uint64_t heap_bits = 0;
    uint64_t null_value = 0;
};

enum class RootProofStage : uint8_t {
    kNotStarted = 0,
    kThreadCore,
    kOwner,
    kRegisterSemantics,
    kCanonicalNull,
    kGlobalObjectPool,
    kIsolateGroup,
    kClassTable,
    kObjectStore,
    kDartCore,
    kComplete,
};

const char* RootProofStageName(RootProofStage stage);

struct RootProofInput {
    const RuntimeProfileRecord* profile = nullptr;
    uint64_t thread = 0;
    uint64_t current_isolate = 0;
    uint64_t canonical_null = 0;
    bool require_current_isolate = false;
    bool require_canonical_null = false;
    bool require_dart_core = true;
    RegisterEvidence registers{};
};

struct RootProof {
    RootProofStage stage = RootProofStage::kNotStarted;
    bool passed = false;
    bool owner_match = false;
    bool canonical_null_match = false;
    bool heap_bits_match = false;
    bool null_register_match = false;
    bool thread_pool_match = false;
    bool register_semantics_match = false;
    bool dart_core_found = false;
    uint64_t heap_base = 0;
    uint64_t isolate = 0;
    uint64_t isolate_group = 0;
    uint64_t thread_null = 0;
    uint64_t global_object_pool = 0;
    uint64_t class_table = 0;
    uint64_t cached_class_table_table = 0;
    uint64_t object_store = 0;
    uint64_t object_pool_length = 0;
    uint64_t num_cids = 0;
    uint64_t libraries = 0;
    uint64_t library_count = 0;
};

struct GeneratedTransitionProof {
    bool passed = false;
    uint64_t execution_state = 0;
    uint64_t top_exit_frame = 0;
    uint64_t vm_tag = 0;
    uint64_t exit_through_ffi = 0;
};

struct FunctionCodeEntryProof {
    bool passed = false;
    bool function_is_function = false;
    bool code_is_code = false;
    bool function_code_match = false;
    bool code_owner_match = false;
    bool shared_code_owner_allowed = false;
    uint64_t function_code = 0;
    uint64_t code_owner = 0;
    uint64_t function_entry = 0;
    uint64_t code_entry = 0;
};

struct ArgumentsDescriptorProof {
    bool passed = false;
    uint32_t type_args_len = 0;
    uint32_t count = 0;
    uint32_t size = 0;
    uint32_t positional_count = 0;
    uint32_t named_count = 0;
};

struct FunctionTypeProof {
    bool passed = false;
    uint64_t signature = 0;
    uint32_t implicit_parameter_count = 0;
    uint32_t fixed_parameter_count = 0;
    uint32_t optional_parameter_count = 0;
    uint32_t parameter_count = 0;
    uint32_t parent_type_argument_count = 0;
    uint32_t type_parameter_count = 0;
    bool has_named_optional_parameters = false;
};

bool DecodePositiveCompressedSmi(uint32_t raw, const RawObjectLayout& layout, uint32_t* out_value);

// Performs bounded, read-only validation of the Dart Thread -> IsolateGroup ->
// ClassTable/ObjectStore root graph for one source-verified ABI candidate.
// No offsets are inferred or synthesized by this function.
RootProof ProveRuntimeRoots(const RootProofInput& input);

// Verifies that a source-verified Thread transition layout is currently in a
// generated-Dart state that is safe to mutate. This is deliberately read-only;
// callers must not publish a mutation-capable transition before it passes.
GeneratedTransitionProof ProveGeneratedTransitionState(const RuntimeProfileRecord& profile,
                                                       uint64_t thread);

// Relational proof for a retained Function -> Code pair. The expected entry is
// checked against both raw objects; owner mismatch is accepted only when the
// caller supplies independent dedup/shared-code evidence.
FunctionCodeEntryProof ProveFunctionCodeEntry(
    const RuntimeProfileRecord& profile, uint64_t heap_base, uint64_t function, uint64_t code,
    uint64_t expected_entry, bool allow_shared_code_owner,
    DartPlantEntryKind entry_kind = DARTPLANT_ENTRY_DEFAULT);

ArgumentsDescriptorProof ProveArgumentsDescriptorLayout(const RuntimeProfileRecord& profile,
                                                        uint64_t heap_base, uint64_t descriptor);
FunctionTypeProof ProveFunctionTypeLayout(const RuntimeProfileRecord& profile, uint64_t heap_base,
                                          uint64_t function);
bool ProveClosureCallRelation(const RuntimeProfileRecord& profile, uint64_t heap_base,
                              uint64_t function, uint64_t descriptor, uint64_t type_arguments);

}  // namespace dartplant::vm_abi

#endif  // DARTPLANT_VM_ABI_PROOF_H_
