// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/abi/proof.h"

#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "vm/dart_string.h"

namespace dartplant::vm_abi {
namespace {

constexpr uint64_t kMaxObjectPoolEntries = 1ULL << 24;
constexpr uint64_t kMaxClassFunctions = 1ULL << 20;
constexpr uint64_t kMaxLibrariesToScan = 4096;
constexpr uint32_t kMaxCallParameters = 1U << 16;
// ARM64 compiler::target::kNumParameterFlagsPerElement.
constexpr uint32_t kNamedParameterFlagsPerSmi = 16;

template <typename T>
bool ReadSelf(uintptr_t address, T* output) {
    if (address == 0 || output == nullptr) return false;
#if defined(__linux__) && defined(SYS_process_vm_readv)
    iovec local = {.iov_base = output, .iov_len = sizeof(T)};
    iovec remote = {.iov_base = reinterpret_cast<void*>(address), .iov_len = sizeof(T)};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(sizeof(T));
#else
    (void) address;
    return false;
#endif
}

bool ReadBytes(uintptr_t address, void* output, size_t size) {
    if (address == 0 || output == nullptr || size == 0) return false;
#if defined(__linux__) && defined(SYS_process_vm_readv)
    iovec local = {.iov_base = output, .iov_len = size};
    iovec remote = {.iov_base = reinterpret_cast<void*>(address), .iov_len = size};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(size);
#else
    (void) address;
    (void) size;
    return false;
#endif
}

uintptr_t CanonicalNativePointer(uint64_t pointer) {
#if defined(__aarch64__)
    return static_cast<uintptr_t>(pointer & 0x00ffffffffffffffULL);
#else
    return static_cast<uintptr_t>(pointer);
#endif
}

bool ReadNativePointer(uintptr_t address, uint64_t* output) {
    if (output == nullptr) return false;
    uint64_t raw = 0;
    if (!ReadSelf(address, &raw) || raw == 0) return false;
    *output = CanonicalNativePointer(raw);
    return *output != 0;
}

bool IsHeapObject(const RuntimeProfileRecord& profile, uint64_t tagged) {
    return (tagged & profile.raw_object.smi_tag_mask) == profile.raw_object.heap_object_tag &&
           tagged >= profile.raw_object.heap_object_tag;
}

uintptr_t Untag(const RuntimeProfileRecord& profile, uint64_t tagged) {
    return tagged < profile.raw_object.heap_object_tag
               ? 0
               : static_cast<uintptr_t>(tagged - profile.raw_object.heap_object_tag);
}

bool TaggedInHeapWindow(const RuntimeProfileRecord& profile, uint64_t heap_base, uint64_t tagged) {
    return heap_base != 0 && IsHeapObject(profile, tagged) &&
           heap_base <= UINT64_MAX - profile.raw_object.heap_object_tag &&
           tagged >= heap_base + profile.raw_object.heap_object_tag &&
           tagged - heap_base <= UINT32_MAX;
}

bool ReadCid(const RuntimeProfileRecord& profile, uint64_t tagged, uint32_t* output) {
    if (output == nullptr || profile.raw_object.class_id_tag_bits == 0 ||
        profile.raw_object.class_id_tag_bits >= 64 || !IsHeapObject(profile, tagged)) {
        return false;
    }
    uint64_t tags = 0;
    if (!ReadSelf(Untag(profile, tagged), &tags)) return false;
    const uint64_t mask = (uint64_t{1} << profile.raw_object.class_id_tag_bits) - 1;
    *output = static_cast<uint32_t>((tags >> profile.raw_object.class_id_tag_shift) & mask);
    return true;
}

bool RequireCid(const RuntimeProfileRecord& profile, uint64_t tagged, uint32_t cid) {
    uint32_t actual = 0;
    return ReadCid(profile, tagged, &actual) && actual == cid;
}

bool ReadPositiveCompressedSmi(const RuntimeProfileRecord& profile, uintptr_t address,
                               uint64_t* output) {
    if (output == nullptr || profile.raw_object.compressed_word_size != sizeof(uint32_t)) {
        return false;
    }
    uint32_t raw = 0;
    uint32_t decoded = 0;
    if (!ReadSelf(address, &raw) ||
        !DecodePositiveCompressedSmi(raw, profile.raw_object, &decoded)) {
        return false;
    }
    *output = decoded;
    return true;
}

bool ReadCompressedObject(const RuntimeProfileRecord& profile, uint64_t heap_base, uintptr_t object,
                          uint32_t offset, uint64_t* output) {
    if (output == nullptr || profile.raw_object.compressed_word_size != sizeof(uint32_t)) {
        return false;
    }
    uint32_t compressed = 0;
    if (!ReadSelf(object + offset, &compressed) ||
        (compressed & profile.raw_object.smi_tag_mask) != profile.raw_object.heap_object_tag ||
        heap_base > UINT64_MAX - compressed) {
        return false;
    }
    *output = heap_base + compressed;
    return true;
}

bool ReadArrayElement(const RuntimeProfileRecord& profile, uint64_t heap_base,
                      uint64_t tagged_array, uint64_t index, uint64_t* output) {
    if (output == nullptr) return false;
    uint32_t cid = 0;
    if (!ReadCid(profile, tagged_array, &cid) ||
        (cid != profile.live_vm.cid_array && cid != profile.live_vm.cid_immutable_array)) {
        return false;
    }
    const uintptr_t array = Untag(profile, tagged_array);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(profile, array + profile.live_vm.array_length_offset, &length) ||
        index >= length || profile.raw_object.compressed_word_size != sizeof(uint32_t)) {
        return false;
    }
    uint32_t compressed = 0;
    if (!ReadSelf(array + profile.live_vm.array_elements_offset + index * sizeof(uint32_t),
                  &compressed) ||
        (compressed & profile.raw_object.smi_tag_mask) != profile.raw_object.heap_object_tag ||
        heap_base > UINT64_MAX - compressed) {
        return false;
    }
    *output = heap_base + compressed;
    return true;
}

bool ReadDartString(const RuntimeProfileRecord& profile, uint64_t tagged, char* output,
                    size_t capacity) {
    return ReadDartStringUtf8(
        profile, tagged,
        [](uintptr_t address, void* output_bytes, size_t size) {
            return ReadBytes(address, output_bytes, size);
        },
        output, capacity);
}

bool ReadAbstractType(const RuntimeProfileRecord& profile, uint64_t heap_base, uint64_t tagged) {
    if (!TaggedInHeapWindow(profile, heap_base, tagged)) return false;
    const auto& layout = profile.function_type;
    uint32_t cid = 0;
    if (!ReadCid(profile, tagged, &cid) ||
        (cid != layout.cid_type && cid != layout.cid_function_type &&
         cid != layout.cid_record_type && cid != layout.cid_type_parameter)) {
        return false;
    }
    uint32_t flags = 0;
    if (!ReadSelf(Untag(profile, tagged) + layout.abstract_type_flags_offset, &flags)) return false;
    if (cid == layout.cid_type) {
        if (layout.type_class_id_shift >= 32 || profile.raw_object.class_id_tag_bits == 0 ||
            profile.raw_object.class_id_tag_bits >= 32) {
            return false;
        }
        const uint32_t mask = (uint32_t{1} << profile.raw_object.class_id_tag_bits) - 1;
        return ((flags >> layout.type_class_id_shift) & mask) != 0;
    }
    if (cid == layout.cid_type_parameter) {
        uint16_t base = 0;
        uint16_t index = 0;
        return ReadSelf(Untag(profile, tagged) + layout.type_parameter_base_offset, &base) &&
               ReadSelf(Untag(profile, tagged) + layout.type_parameter_index_offset, &index);
    }
    return true;
}

struct NamedParameterProof {
    std::string name;
    bool required = false;
};

struct FunctionTypeDetails {
    FunctionTypeProof proof{};
    std::vector<NamedParameterProof> named;
};

bool ReadFunctionTypeDetails(const RuntimeProfileRecord& profile, uint64_t heap_base,
                             uint64_t function, FunctionTypeDetails* output) {
    if (output == nullptr || !TaggedInHeapWindow(profile, heap_base, function) ||
        !RequireCid(profile, function, profile.live_vm.cid_function)) {
        return false;
    }
    const auto& layout = profile.function_type;
    uint64_t signature = 0;
    if (!ReadCompressedObject(profile, heap_base, Untag(profile, function),
                              layout.function_signature_offset, &signature) ||
        !TaggedInHeapWindow(profile, heap_base, signature) ||
        !RequireCid(profile, signature, layout.cid_function_type)) {
        return false;
    }
    const uintptr_t object = Untag(profile, signature);
    uint32_t packed_counts = 0;
    uint16_t packed_type_counts = 0;
    uint64_t result_type = 0;
    if (!ReadSelf(object + layout.packed_parameter_counts_offset, &packed_counts) ||
        !ReadSelf(object + layout.packed_type_parameter_counts_offset, &packed_type_counts) ||
        !ReadCompressedObject(profile, heap_base, object, layout.result_type_offset,
                              &result_type) ||
        !ReadAbstractType(profile, heap_base, result_type)) {
        return false;
    }

    FunctionTypeDetails details{};
    details.proof.signature = signature;
    details.proof.implicit_parameter_count = packed_counts & 0x1U;
    details.proof.has_named_optional_parameters = ((packed_counts >> 1) & 0x1U) != 0;
    details.proof.fixed_parameter_count = (packed_counts >> 2) & 0x3fffU;
    details.proof.optional_parameter_count = (packed_counts >> 16) & 0x3fffU;
    details.proof.parameter_count =
        details.proof.fixed_parameter_count + details.proof.optional_parameter_count;
    details.proof.parent_type_argument_count = packed_type_counts & 0xffU;
    details.proof.type_parameter_count = (packed_type_counts >> 8) & 0xffU;
    if (details.proof.parameter_count > kMaxCallParameters ||
        details.proof.implicit_parameter_count > details.proof.fixed_parameter_count ||
        (details.proof.has_named_optional_parameters &&
         details.proof.optional_parameter_count == 0)) {
        return false;
    }
    if (details.proof.parameter_count != 0) {
        uint64_t parameter_types = 0;
        uint64_t parameter_count = 0;
        if (!ReadCompressedObject(profile, heap_base, object, layout.parameter_types_offset,
                                  &parameter_types) ||
            !ReadPositiveCompressedSmi(
                profile, Untag(profile, parameter_types) + profile.live_vm.array_length_offset,
                &parameter_count) ||
            parameter_count != details.proof.parameter_count) {
            return false;
        }
        for (uint32_t index = 0; index < details.proof.parameter_count; ++index) {
            uint64_t parameter_type = 0;
            if (!ReadArrayElement(profile, heap_base, parameter_types, index, &parameter_type) ||
                !ReadAbstractType(profile, heap_base, parameter_type)) {
                return false;
            }
        }
    }
    if (details.proof.has_named_optional_parameters) {
        uint64_t names = 0;
        uint64_t slot_count = 0;
        const uint32_t flag_slots =
            (details.proof.optional_parameter_count + kNamedParameterFlagsPerSmi - 1) /
            kNamedParameterFlagsPerSmi;
        if (!ReadCompressedObject(profile, heap_base, object, layout.named_parameter_names_offset,
                                  &names) ||
            !ReadPositiveCompressedSmi(profile,
                                       Untag(profile, names) + profile.live_vm.array_length_offset,
                                       &slot_count) ||
            slot_count != details.proof.optional_parameter_count + flag_slots) {
            return false;
        }
        details.named.reserve(details.proof.optional_parameter_count);
        for (uint32_t index = 0; index < details.proof.optional_parameter_count; ++index) {
            uint64_t tagged_name = 0;
            char name[256]{};
            if (!ReadArrayElement(profile, heap_base, names, index, &tagged_name) ||
                !ReadDartString(profile, tagged_name, name, sizeof(name))) {
                return false;
            }
            const uint32_t flag_index =
                details.proof.optional_parameter_count + index / kNamedParameterFlagsPerSmi;
            uint32_t raw_flags = 0;
            uint32_t flags = 0;
            if (!ReadSelf(Untag(profile, names) + profile.live_vm.array_elements_offset +
                              static_cast<uintptr_t>(flag_index) * sizeof(uint32_t),
                          &raw_flags) ||
                !DecodePositiveCompressedSmi(raw_flags, profile.raw_object, &flags)) {
                return false;
            }
            details.named.push_back({
                .name = name,
                .required = (flags & (1U << (index % kNamedParameterFlagsPerSmi))) != 0,
            });
        }
    }
    details.proof.passed = true;
    *output = std::move(details);
    return true;
}

struct DescriptorNamedProof {
    std::string name;
    uint32_t position = 0;
};

struct ArgumentsDescriptorDetails {
    ArgumentsDescriptorProof proof{};
    std::vector<DescriptorNamedProof> named;
};

bool ReadArgumentsDescriptorDetails(const RuntimeProfileRecord& profile, uint64_t heap_base,
                                    uint64_t descriptor, ArgumentsDescriptorDetails* output) {
    if (output == nullptr || !TaggedInHeapWindow(profile, heap_base, descriptor)) return false;
    uint32_t cid = 0;
    if (!ReadCid(profile, descriptor, &cid) ||
        (cid != profile.live_vm.cid_array && cid != profile.live_vm.cid_immutable_array)) {
        return false;
    }
    const uintptr_t object = Untag(profile, descriptor);
    const auto& layout = profile.arguments_descriptor;
    uint64_t type_args_len = 0;
    uint64_t count = 0;
    uint64_t size = 0;
    uint64_t positional = 0;
    if (!ReadPositiveCompressedSmi(profile, object + layout.type_args_len_offset, &type_args_len) ||
        !ReadPositiveCompressedSmi(profile, object + layout.count_offset, &count) ||
        !ReadPositiveCompressedSmi(profile, object + layout.size_offset, &size) ||
        !ReadPositiveCompressedSmi(profile, object + layout.positional_count_offset, &positional) ||
        count > kMaxCallParameters || size > kMaxCallParameters || positional > count ||
        count > size) {
        return false;
    }
    ArgumentsDescriptorDetails details{};
    details.proof.type_args_len = static_cast<uint32_t>(type_args_len);
    details.proof.count = static_cast<uint32_t>(count);
    details.proof.size = static_cast<uint32_t>(size);
    details.proof.positional_count = static_cast<uint32_t>(positional);
    details.proof.named_count = static_cast<uint32_t>(count - positional);
    details.named.reserve(details.proof.named_count);
    std::vector<uint32_t> positions;
    positions.reserve(details.proof.named_count);
    for (uint32_t index = 0; index < details.proof.named_count; ++index) {
        const uintptr_t entry = object + layout.first_named_entry_offset +
                                static_cast<uintptr_t>(index) * layout.named_entry_size;
        uint64_t tagged_name = 0;
        uint64_t position = 0;
        char name[256]{};
        if (!ReadCompressedObject(profile, heap_base, entry, layout.name_offset, &tagged_name) ||
            !ReadDartString(profile, tagged_name, name, sizeof(name)) ||
            !ReadPositiveCompressedSmi(profile, entry + layout.position_offset, &position) ||
            position < positional || position >= count ||
            std::find(positions.begin(), positions.end(), position) != positions.end()) {
            return false;
        }
        positions.push_back(static_cast<uint32_t>(position));
        details.named.push_back({.name = name, .position = static_cast<uint32_t>(position)});
    }
    details.proof.passed = true;
    *output = std::move(details);
    return true;
}

bool LibraryUriEquals(const RuntimeProfileRecord& profile, uint64_t heap_base,
                      uint64_t tagged_library, const char* expected) {
    if (!RequireCid(profile, tagged_library, profile.live_vm.cid_library)) return false;
    uint64_t tagged_url = 0;
    char uri[64]{};
    return ReadCompressedObject(profile, heap_base, Untag(profile, tagged_library),
                                profile.live_vm.library_url_offset, &tagged_url) &&
           ReadDartString(profile, tagged_url, uri, sizeof(uri)) && std::strcmp(uri, expected) == 0;
}

}  // namespace

bool DecodePositiveCompressedSmi(uint32_t raw, const RawObjectLayout& layout, uint32_t* out_value) {
    if (out_value == nullptr || layout.compressed_word_size != sizeof(uint32_t) ||
        layout.smi_tag_shift >= 32 || (raw & layout.smi_tag_mask) != layout.smi_tag) {
        return false;
    }
    const int32_t decoded = static_cast<int32_t>(raw) >> layout.smi_tag_shift;
    if (decoded < 0) return false;
    *out_value = static_cast<uint32_t>(decoded);
    return true;
}

ArgumentsDescriptorProof ProveArgumentsDescriptorLayout(const RuntimeProfileRecord& profile,
                                                        uint64_t heap_base, uint64_t descriptor) {
    ArgumentsDescriptorDetails details{};
    if (!ReadArgumentsDescriptorDetails(profile, heap_base, descriptor, &details)) return {};
    return details.proof;
}

FunctionTypeProof ProveFunctionTypeLayout(const RuntimeProfileRecord& profile, uint64_t heap_base,
                                          uint64_t function) {
    FunctionTypeDetails details{};
    if (!ReadFunctionTypeDetails(profile, heap_base, function, &details)) return {};
    return details.proof;
}

bool ProveClosureCallRelation(const RuntimeProfileRecord& profile, uint64_t heap_base,
                              uint64_t function, uint64_t descriptor, uint64_t type_arguments) {
    FunctionTypeDetails signature{};
    ArgumentsDescriptorDetails arguments{};
    if (!ReadFunctionTypeDetails(profile, heap_base, function, &signature) ||
        !ReadArgumentsDescriptorDetails(profile, heap_base, descriptor, &arguments)) {
        return false;
    }
    const auto& function_type = signature.proof;
    const auto& call = arguments.proof;
    if (function_type.implicit_parameter_count > 1 || call.count == 0 ||
        call.positional_count == 0 || call.size != call.count ||
        function_type.fixed_parameter_count < function_type.implicit_parameter_count) {
        return false;
    }
    const uint32_t user_arguments = call.count - 1;
    const uint32_t positional_user_arguments = call.positional_count - 1;
    const uint32_t fixed_user_parameters =
        function_type.fixed_parameter_count - function_type.implicit_parameter_count;
    const uint32_t user_parameters =
        function_type.parameter_count - function_type.implicit_parameter_count;
    if (user_arguments > user_parameters ||
        (call.type_args_len != 0 && call.type_args_len != function_type.type_parameter_count)) {
        return false;
    }
    if (call.type_args_len == 0) {
        if (type_arguments != 0) return false;
    } else {
        uint64_t length = 0;
        if (!TaggedInHeapWindow(profile, heap_base, type_arguments) ||
            !RequireCid(profile, type_arguments, profile.type_arguments.cid) ||
            !ReadPositiveCompressedSmi(
                profile, Untag(profile, type_arguments) + profile.type_arguments.length_offset,
                &length) ||
            length != call.type_args_len) {
            return false;
        }
    }
    if (!function_type.has_named_optional_parameters) {
        return call.named_count == 0 && positional_user_arguments == user_arguments &&
               positional_user_arguments >= fixed_user_parameters &&
               positional_user_arguments <= user_parameters;
    }
    if (positional_user_arguments != fixed_user_parameters ||
        call.named_count > function_type.optional_parameter_count) {
        return false;
    }
    for (const auto& actual : arguments.named) {
        if (std::none_of(signature.named.begin(), signature.named.end(),
                         [&actual](const NamedParameterProof& formal) {
                             return formal.name == actual.name;
                         })) {
            return false;
        }
    }
    return std::all_of(signature.named.begin(), signature.named.end(),
                       [&arguments](const NamedParameterProof& formal) {
                           return !formal.required ||
                                  std::any_of(arguments.named.begin(), arguments.named.end(),
                                              [&formal](const DescriptorNamedProof& actual) {
                                                  return actual.name == formal.name;
                                              });
                       });
}

const char* RootProofStageName(RootProofStage stage) {
    switch (stage) {
    case RootProofStage::kNotStarted:
        return "not-started";
    case RootProofStage::kThreadCore:
        return "thread-core";
    case RootProofStage::kOwner:
        return "owner";
    case RootProofStage::kRegisterSemantics:
        return "register-semantics";
    case RootProofStage::kCanonicalNull:
        return "canonical-null";
    case RootProofStage::kGlobalObjectPool:
        return "global-object-pool";
    case RootProofStage::kIsolateGroup:
        return "isolate-group";
    case RootProofStage::kClassTable:
        return "class-table";
    case RootProofStage::kObjectStore:
        return "object-store";
    case RootProofStage::kDartCore:
        return "dart-core";
    case RootProofStage::kComplete:
        return "complete";
    }
    return "unknown";
}

RootProof ProveRuntimeRoots(const RootProofInput& input) {
    RootProof proof{};
    if (input.profile == nullptr || input.thread == 0) return proof;
    const RuntimeProfileRecord& profile = *input.profile;
    const DartPlantLiveVmProfile& vm = profile.live_vm;

    proof.stage = RootProofStage::kThreadCore;
    if (!ReadSelf(static_cast<uintptr_t>(input.thread) + vm.thread_heap_base_offset,
                  &proof.heap_base) ||
        !ReadSelf(static_cast<uintptr_t>(input.thread) + vm.thread_object_null_offset,
                  &proof.thread_null) ||
        !ReadSelf(static_cast<uintptr_t>(input.thread) + vm.thread_global_object_pool_offset,
                  &proof.global_object_pool) ||
        !ReadNativePointer(static_cast<uintptr_t>(input.thread) + vm.thread_isolate_offset,
                           &proof.isolate) ||
        !ReadNativePointer(static_cast<uintptr_t>(input.thread) + vm.thread_isolate_group_offset,
                           &proof.isolate_group)) {
        return proof;
    }

    proof.stage = RootProofStage::kOwner;
    proof.owner_match = input.current_isolate == 0 || proof.isolate == input.current_isolate;
    if (input.require_current_isolate && !proof.owner_match) return proof;

    proof.stage = RootProofStage::kRegisterSemantics;
    if (input.registers.available) {
        proof.heap_bits_match = static_cast<uint32_t>(input.registers.heap_bits) ==
                                static_cast<uint32_t>(proof.heap_base >> 32);
        proof.null_register_match = input.registers.null_value == proof.thread_null;
        proof.thread_pool_match =
            IsHeapObject(profile, proof.global_object_pool) &&
            proof.global_object_pool >= profile.raw_object.heap_object_tag &&
            input.registers.pp == proof.global_object_pool - profile.raw_object.heap_object_tag;
        proof.register_semantics_match =
            proof.heap_bits_match && proof.null_register_match && proof.thread_pool_match;
        if (!proof.register_semantics_match) return proof;
    }

    proof.stage = RootProofStage::kCanonicalNull;
    proof.canonical_null_match =
        input.canonical_null == 0 || proof.thread_null == input.canonical_null;
    if ((input.require_canonical_null && !proof.canonical_null_match) || proof.heap_base == 0 ||
        !TaggedInHeapWindow(profile, proof.heap_base, proof.thread_null) ||
        !RequireCid(profile, proof.thread_null, profile.function_type.cid_null)) {
        return proof;
    }

    proof.stage = RootProofStage::kGlobalObjectPool;
    if (!TaggedInHeapWindow(profile, proof.heap_base, proof.global_object_pool) ||
        !RequireCid(profile, proof.global_object_pool, vm.cid_object_pool) ||
        !ReadSelf(Untag(profile, proof.global_object_pool) + vm.object_pool_length_offset,
                  &proof.object_pool_length) ||
        proof.object_pool_length == 0 || proof.object_pool_length > kMaxObjectPoolEntries) {
        return proof;
    }

    proof.stage = RootProofStage::kIsolateGroup;
    if (!ReadNativePointer(
            static_cast<uintptr_t>(proof.isolate_group) + vm.isolate_group_class_table_offset,
            &proof.class_table) ||
        !ReadNativePointer(static_cast<uintptr_t>(proof.isolate_group) +
                               vm.isolate_group_cached_class_table_table_offset,
                           &proof.cached_class_table_table) ||
        !ReadNativePointer(
            static_cast<uintptr_t>(proof.isolate_group) + vm.isolate_group_object_store_offset,
            &proof.object_store)) {
        return proof;
    }

    proof.stage = RootProofStage::kClassTable;
    const uint64_t max_cids = profile.raw_object.class_id_tag_bits >= 63
                                  ? 0
                                  : uint64_t{1} << profile.raw_object.class_id_tag_bits;
    const uint32_t highest_required_cid =
        std::max({vm.cid_class, vm.cid_library, vm.cid_object_pool, vm.cid_two_byte_string,
                  profile.type_arguments.cid, profile.function_type.cid_never});
    uint64_t class_class = 0;
    if (max_cids == 0 ||
        !ReadSelf(static_cast<uintptr_t>(proof.class_table) + vm.class_table_num_cids_offset,
                  &proof.num_cids) ||
        proof.num_cids <= highest_required_cid || proof.num_cids > max_cids ||
        !ReadSelf(static_cast<uintptr_t>(proof.cached_class_table_table) +
                      static_cast<uintptr_t>(vm.cid_class) * sizeof(uint64_t),
                  &class_class) ||
        !TaggedInHeapWindow(profile, proof.heap_base, class_class) ||
        !RequireCid(profile, class_class, vm.cid_class)) {
        return proof;
    }

    proof.stage = RootProofStage::kObjectStore;
    if (!ReadSelf(static_cast<uintptr_t>(proof.object_store) + vm.object_store_libraries_offset,
                  &proof.libraries) ||
        !TaggedInHeapWindow(profile, proof.heap_base, proof.libraries) ||
        !RequireCid(profile, proof.libraries, vm.cid_growable_object_array)) {
        return proof;
    }
    const uintptr_t growable = Untag(profile, proof.libraries);
    uint64_t library_data = 0;
    if (!ReadPositiveCompressedSmi(profile, growable + vm.growable_object_array_length_offset,
                                   &proof.library_count) ||
        proof.library_count == 0 || proof.library_count > kMaxClassFunctions ||
        !ReadCompressedObject(profile, proof.heap_base, growable,
                              vm.growable_object_array_data_offset, &library_data)) {
        return proof;
    }
    uint32_t library_data_cid = 0;
    if (!ReadCid(profile, library_data, &library_data_cid) ||
        (library_data_cid != vm.cid_array && library_data_cid != vm.cid_immutable_array)) {
        return proof;
    }

    proof.stage = RootProofStage::kDartCore;
    if (input.require_dart_core) {
        const uint64_t scan_count = std::min(proof.library_count, kMaxLibrariesToScan);
        for (uint64_t index = 0; index < scan_count; ++index) {
            uint64_t library = 0;
            if (ReadArrayElement(profile, proof.heap_base, library_data, index, &library) &&
                LibraryUriEquals(profile, proof.heap_base, library, "dart:core")) {
                proof.dart_core_found = true;
                break;
            }
        }
        if (!proof.dart_core_found) return proof;
    }

    proof.stage = RootProofStage::kComplete;
    proof.passed = true;
    return proof;
}

GeneratedTransitionProof ProveGeneratedTransitionState(const RuntimeProfileRecord& profile,
                                                       uint64_t thread) {
    GeneratedTransitionProof proof{};
    if (thread == 0) return proof;
    const VmThreadBridgeLayout& bridge = profile.thread_bridge;
    const VmTransitionLayout& transition = profile.transition;
    if (bridge.top_exit_frame_offset == 0 || bridge.vm_tag_offset == 0 ||
        bridge.execution_state_offset == 0 || bridge.exit_through_ffi_offset == 0 ||
        transition.vm_tag_dart == 0) {
        return proof;
    }
    const uintptr_t base = static_cast<uintptr_t>(thread);
    if (!ReadSelf(base + bridge.execution_state_offset, &proof.execution_state) ||
        !ReadSelf(base + bridge.top_exit_frame_offset, &proof.top_exit_frame) ||
        !ReadSelf(base + bridge.vm_tag_offset, &proof.vm_tag) ||
        !ReadSelf(base + bridge.exit_through_ffi_offset, &proof.exit_through_ffi)) {
        return proof;
    }
    proof.passed = proof.execution_state == transition.execution_generated &&
                   proof.top_exit_frame == transition.exit_none &&
                   proof.vm_tag == transition.vm_tag_dart &&
                   (proof.exit_through_ffi == transition.exit_none ||
                    proof.exit_through_ffi == transition.exit_through_runtime_call);
    return proof;
}

FunctionCodeEntryProof ProveFunctionCodeEntry(const RuntimeProfileRecord& profile,
                                              uint64_t heap_base, uint64_t function, uint64_t code,
                                              uint64_t expected_entry, bool allow_shared_code_owner,
                                              DartPlantEntryKind entry_kind) {
    FunctionCodeEntryProof proof{};
    if (heap_base == 0 || !TaggedInHeapWindow(profile, heap_base, function) ||
        !TaggedInHeapWindow(profile, heap_base, code)) {
        return proof;
    }
    proof.function_is_function = RequireCid(profile, function, profile.live_vm.cid_function);
    proof.code_is_code = RequireCid(profile, code, profile.live_vm.cid_code);
    if (!proof.function_is_function || !proof.code_is_code) return proof;

    const uintptr_t function_object = Untag(profile, function);
    const uintptr_t code_object = Untag(profile, code);
    uint32_t function_entry_offset = profile.live_vm.function_entry_point_offset;
    uint32_t code_entry_offset = profile.live_vm.code_entry_point_offset;
    bool require_function_entry_match = true;
    if (entry_kind == DARTPLANT_ENTRY_UNCHECKED) {
        function_entry_offset = profile.live_vm.function_unchecked_entry_point_offset;
        code_entry_offset = profile.live_vm.code_unchecked_entry_point_offset;
    } else if (entry_kind == DARTPLANT_ENTRY_MONOMORPHIC) {
        code_entry_offset = profile.live_vm.code_monomorphic_entry_point_offset;
        require_function_entry_match = false;
    } else if (entry_kind == DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED) {
        code_entry_offset = profile.live_vm.code_monomorphic_unchecked_entry_point_offset;
        require_function_entry_match = false;
    } else if (entry_kind != DARTPLANT_ENTRY_DEFAULT) {
        return proof;
    }
    if (!ReadNativePointer(function_object + function_entry_offset, &proof.function_entry) ||
        !ReadNativePointer(code_object + code_entry_offset, &proof.code_entry) ||
        !ReadCompressedObject(profile, heap_base, function_object,
                              profile.live_vm.function_code_offset, &proof.function_code) ||
        !ReadCompressedObject(profile, heap_base, code_object, profile.live_vm.code_owner_offset,
                              &proof.code_owner)) {
        return proof;
    }
    proof.function_code_match = proof.function_code == code;
    proof.code_owner_match = proof.code_owner == function;
    proof.shared_code_owner_allowed =
        allow_shared_code_owner &&
        RequireCid(profile, proof.code_owner, profile.live_vm.cid_function);
    const bool entry_match =
        expected_entry == 0 ||
        (proof.code_entry == expected_entry &&
         (!require_function_entry_match || proof.function_entry == expected_entry));
    proof.passed = entry_match && proof.function_code_match &&
                   (proof.code_owner_match || proof.shared_code_owner_allowed);
    return proof;
}

}  // namespace dartplant::vm_abi
