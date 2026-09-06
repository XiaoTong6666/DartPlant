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

namespace dartplant::vm_abi {
namespace {

constexpr uint64_t kMaxObjectPoolEntries = 1ULL << 24;
constexpr uint64_t kMaxLibrariesToScan = 4096;

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
    if (!ReadSelf(address, &raw) ||
        (raw & profile.raw_object.smi_tag_mask) != profile.raw_object.smi_tag) {
        return false;
    }
    *output = static_cast<uint64_t>(raw >> profile.raw_object.smi_tag_shift);
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
    if (output == nullptr || capacity == 0) return false;
    uint32_t cid = 0;
    if (!ReadCid(profile, tagged, &cid) || (cid != profile.live_vm.cid_one_byte_string &&
                                            cid != profile.live_vm.cid_two_byte_string)) {
        return false;
    }
    const uintptr_t string = Untag(profile, tagged);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(profile, string + profile.live_vm.string_length_offset,
                                   &length) ||
        length >= capacity) {
        return false;
    }
    if (cid == profile.live_vm.cid_one_byte_string) {
        if (!ReadBytes(string + profile.live_vm.string_data_offset, output,
                       static_cast<size_t>(length))) {
            return false;
        }
        output[length] = '\0';
        return true;
    }
    if (length > (std::numeric_limits<size_t>::max() / sizeof(uint16_t))) return false;
    std::array<uint16_t, 256> units{};
    if (length >= units.size() ||
        !ReadBytes(string + profile.live_vm.string_data_offset, units.data(),
                   static_cast<size_t>(length) * sizeof(uint16_t))) {
        return false;
    }
    size_t cursor = 0;
    for (size_t index = 0; index < length; ++index) {
        const uint16_t unit = units[index];
        if (unit > 0x7f || cursor + 1 >= capacity) return false;
        output[cursor++] = static_cast<char>(unit);
    }
    output[cursor] = '\0';
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
        proof.register_semantics_match =
            static_cast<uint32_t>(input.registers.heap_bits) ==
                static_cast<uint32_t>(proof.heap_base >> 32) &&
            input.registers.null_value == proof.thread_null &&
            IsHeapObject(profile, proof.global_object_pool) &&
            proof.global_object_pool >= profile.raw_object.heap_object_tag &&
            input.registers.pp == proof.global_object_pool - profile.raw_object.heap_object_tag;
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
        proof.library_count == 0 || proof.library_count > kMaxObjectPoolEntries ||
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

}  // namespace dartplant::vm_abi
