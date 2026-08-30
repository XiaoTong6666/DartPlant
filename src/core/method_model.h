// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_CORE_METHOD_MODEL_H_
#define DARTPLANT_CORE_METHOD_MODEL_H_

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dartplant/advanced/flutter_snapshot.h"
#include "dartplant/dartplant.h"

namespace dartplant {

struct Arm64ReturnPatch {
    uintptr_t address = 0;
    uint32_t original_instruction = 0;
    uint32_t patched_instruction = 0;
    uint32_t consumers = 0;
};

struct DartMethodIdentity {
    std::string library_uri;
    std::string class_name;
    std::string function_name;
    std::string signature;
    DartPlantEntryKind entry_kind = DARTPLANT_ENTRY_DEFAULT;

    bool operator==(const DartMethodIdentity& other) const {
        return library_uri == other.library_uri && class_name == other.class_name &&
               function_name == other.function_name && signature == other.signature &&
               entry_kind == other.entry_kind;
    }
};

inline bool SameLogicalFunctionIdentity(const DartMethodIdentity& left,
                                        const DartMethodIdentity& right) {
    return left.library_uri == right.library_uri && left.class_name == right.class_name &&
           left.function_name == right.function_name && left.signature == right.signature;
}

enum class DartFunctionSource {
    // Legacy process-global metadata path only.
    kLegacyMetadata,
    // Exact artifact-bound snapshot sidecar used only when PRODUCT AOT has
    // dropped the logical Function object from the live heap.
    kOfflineSnapshotIndex,
    kLiveVm,
    // Native unit/device fixtures that exercise the hook backend without a
    // Dart VM. Never produced by runtime method resolution.
    kSynthetic,
};

// One Dart Code object/payload can expose multiple physical entry points
// (normal, unchecked, monomorphic and monomorphic-unchecked). Patch ownership
// that affects the shared body therefore belongs here rather than to an
// individual entry hook.
struct DartCodePayload {
    using Id = uintptr_t;

    mutable std::mutex mutex;
    Id id = 0;
    uintptr_t start = 0;
    uint32_t instructions_length = 0;
    uint64_t code_object = 0;
    // Legacy metadata only knows the entry and code size. A live VM or exact
    // artifact range can promote that provisional identity before a hook is
    // installed, but an active hook must never be moved to another payload.
    bool exact_identity = false;

    // Captured from the managed-patch-normalized image before the first return
    // interception is installed. Entry-specific CFG walks always use this
    // immutable view so an already-patched sibling cannot hide a RET.
    std::vector<uint8_t> pristine_bytes;
    void* return_entry = nullptr;
    size_t return_entry_size = 0;
    // Once any live instruction has branched to return_entry, the veneer (and
    // this payload object carrying the raw dispatcher cookie) cannot be freed
    // merely because the current managed consumer count reaches zero. An
    // unhooked sibling entry may have fetched the published B already without
    // ever incrementing a HookRecord::in_flight counter.
    bool return_entry_published = false;
    std::vector<Arm64ReturnPatch> return_patches;
    uint32_t return_interception_consumers = 0;

    bool HasExactIdentity() const {
        std::lock_guard lock(mutex);
        return exact_identity;
    }

    uintptr_t end() const {
        return start > UINTPTR_MAX - instructions_length ? 0 : start + instructions_length;
    }

    bool Contains(uintptr_t address, size_t size = 1) const {
        const uintptr_t payload_end = end();
        return start != 0 && payload_end > start && address >= start && address <= payload_end &&
               size <= payload_end - address;
    }
};

// Physical entry target inside one DartCodePayload. Logical Function aliasing
// is entry-specific, while Code body/RET ownership is payload-specific.
struct DartEntryTarget {
    using Id = uintptr_t;

    mutable std::mutex mutex;
    Id id = 0;
    uintptr_t entry = 0;
    uint32_t code_size = 0;
    uint64_t code_object = 0;
    uint32_t reported_alias_count = 1;
    DartPlantCodeIdentityProof identity_proof = DARTPLANT_CODE_IDENTITY_UNKNOWN;
    std::shared_ptr<DartCodePayload> payload;
    std::vector<DartMethodIdentity> aliases;
    DartPlantHook* hook_record = nullptr;

    bool MergeEvidence(
        uint32_t new_code_size, uint64_t new_code_object, uint32_t alias_count,
        DartPlantCodeIdentityProof new_identity_proof = DARTPLANT_CODE_IDENTITY_UNKNOWN) {
        std::lock_guard lock(mutex);
        if ((code_size != 0 && new_code_size != 0 && code_size != new_code_size) ||
            (code_object != 0 && new_code_object != 0 && code_object != new_code_object)) {
            return false;
        }
        if (code_size == 0) code_size = new_code_size;
        if (code_object == 0) code_object = new_code_object;
        if (alias_count > reported_alias_count) reported_alias_count = alias_count;
        if (new_identity_proof == DARTPLANT_CODE_IDENTITY_SHARED || reported_alias_count > 1) {
            identity_proof = DARTPLANT_CODE_IDENTITY_SHARED;
        } else if (new_identity_proof == DARTPLANT_CODE_IDENTITY_UNIQUE &&
                   identity_proof == DARTPLANT_CODE_IDENTITY_UNKNOWN) {
            identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
        }
        return true;
    }

    void AddAlias(const DartMethodIdentity& identity) {
        std::lock_guard lock(mutex);
        const auto found = std::find(aliases.begin(), aliases.end(), identity);
        if (found == aliases.end()) aliases.push_back(identity);
        if (DistinctLogicalFunctionAliasCountLocked() > 1) {
            reported_alias_count =
                std::max<uint32_t>(reported_alias_count, DistinctLogicalFunctionAliasCountLocked());
            identity_proof = DARTPLANT_CODE_IDENTITY_SHARED;
        }
    }

    uint32_t KnownAliasCount() const {
        std::lock_guard lock(mutex);
        return static_cast<uint32_t>(aliases.size());
    }

    uint32_t AliasCount() const {
        std::lock_guard lock(mutex);
        return std::max<uint32_t>(reported_alias_count, static_cast<uint32_t>(aliases.size()));
    }

    std::vector<DartMethodIdentity> AliasSnapshot() const {
        std::lock_guard lock(mutex);
        return aliases;
    }

    bool HasAlias(const DartMethodIdentity& identity) const {
        std::lock_guard lock(mutex);
        return std::find(aliases.begin(), aliases.end(), identity) != aliases.end();
    }

    void BindHookRecord(DartPlantHook* hook) {
        std::lock_guard lock(mutex);
        hook_record = hook;
    }

    void UnbindHookRecord(const DartPlantHook* hook) {
        std::lock_guard lock(mutex);
        if (hook_record == hook) hook_record = nullptr;
    }

    DartPlantHook* HookRecord() const {
        std::lock_guard lock(mutex);
        return hook_record;
    }

    std::shared_ptr<DartCodePayload> Payload() const {
        std::lock_guard lock(mutex);
        return payload;
    }

    bool UpgradePayload(const std::shared_ptr<DartCodePayload>& replacement) {
        if (replacement == nullptr) return false;
        std::lock_guard lock(mutex);
        if (hook_record != nullptr) return false;
        payload = replacement;
        return true;
    }

    bool IsShared() const {
        std::lock_guard lock(mutex);
        return identity_proof == DARTPLANT_CODE_IDENTITY_SHARED || reported_alias_count > 1 ||
               DistinctLogicalFunctionAliasCountLocked() > 1;
    }

    bool HasProvenUniqueIdentity() const {
        std::lock_guard lock(mutex);
        return identity_proof == DARTPLANT_CODE_IDENTITY_UNIQUE && reported_alias_count <= 1 &&
               DistinctLogicalFunctionAliasCountLocked() <= 1;
    }

private:
    uint32_t DistinctLogicalFunctionAliasCountLocked() const {
        uint32_t count = 0;
        for (size_t index = 0; index < aliases.size(); ++index) {
            bool seen = false;
            for (size_t earlier = 0; earlier < index; ++earlier) {
                if (SameLogicalFunctionIdentity(aliases[index], aliases[earlier])) {
                    seen = true;
                    break;
                }
            }
            if (!seen) ++count;
        }
        return count;
    }
};

struct DartFunctionHandle {
    DartMethodIdentity identity;
    uint64_t function_object = 0;
    uint64_t code_object = 0;
    DartFunctionSource source = DartFunctionSource::kLegacyMetadata;
    uint32_t function_kind = 0;
    uint32_t runtime_profile_version = 0;
    bool closure_call_entry_only = false;
    uint32_t thread_jump_to_frame_entry_point_offset = 0;
    std::shared_ptr<DartEntryTarget> code_target;

    DartEntryTarget::Id EntryTargetId() const {
        return code_target == nullptr ? 0 : code_target->id;
    }
};

class DartEntryTargetRegistry final {
public:
    std::shared_ptr<DartEntryTarget> GetOrCreate(
        uintptr_t entry, uint32_t code_size, uint64_t code_object = 0,
        uint32_t reported_alias_count = 1,
        DartPlantCodeIdentityProof identity_proof = DARTPLANT_CODE_IDENTITY_UNKNOWN,
        uintptr_t payload_start = 0, uint32_t instructions_length = 0) {
        if (entry == 0) return nullptr;
        const bool explicit_payload_identity = payload_start != 0 || instructions_length != 0;
        if (payload_start == 0) payload_start = entry;
        if (payload_start > entry) return nullptr;
        if (instructions_length == 0) {
            const uintptr_t prefix = entry - payload_start;
            if (prefix > UINT32_MAX || code_size > UINT32_MAX - prefix) return nullptr;
            instructions_length = static_cast<uint32_t>(prefix) + code_size;
        }
        if (instructions_length == 0 || payload_start > UINTPTR_MAX - instructions_length ||
            entry < payload_start || entry >= payload_start + instructions_length ||
            code_size > payload_start + instructions_length - entry) {
            return nullptr;
        }
        if (explicit_payload_identity && code_size != payload_start + instructions_length - entry) {
            return nullptr;
        }
        std::lock_guard lock(mutex_);
        auto target_found = targets_.find(entry);
        if (target_found != targets_.end()) {
            if (auto existing = target_found->second.lock(); existing != nullptr) {
                auto existing_payload = existing->Payload();
                if (existing_payload == nullptr) return nullptr;
                if (!explicit_payload_identity) {
                    return existing->MergeEvidence(code_size, code_object, reported_alias_count,
                                                   identity_proof)
                               ? existing
                               : nullptr;
                }
                if (existing_payload->start != payload_start ||
                    existing_payload->instructions_length != instructions_length) {
                    if (existing_payload->HasExactIdentity() || existing->HookRecord() != nullptr) {
                        return nullptr;
                    }
                    auto replacement = FindOrCreatePayloadLocked(payload_start, instructions_length,
                                                                 code_object, true);
                    if (replacement == nullptr ||
                        !existing->MergeEvidence(code_size, code_object, reported_alias_count,
                                                 identity_proof) ||
                        !existing->UpgradePayload(replacement)) {
                        return nullptr;
                    }
                    return existing;
                }
                if (!existing->MergeEvidence(code_size, code_object, reported_alias_count,
                                             identity_proof)) {
                    return nullptr;
                }
                if (explicit_payload_identity) {
                    std::lock_guard payload_lock(existing_payload->mutex);
                    existing_payload->exact_identity = true;
                }
                return existing;
            }
        }

        auto payload = FindOrCreatePayloadLocked(payload_start, instructions_length, code_object,
                                                 explicit_payload_identity);
        if (payload == nullptr) return nullptr;

        auto target = std::make_shared<DartEntryTarget>();
        target->id = entry;
        target->entry = entry;
        target->code_size = code_size;
        target->code_object = code_object;
        target->payload = std::move(payload);
        target->reported_alias_count = std::max<uint32_t>(1, reported_alias_count);
        target->identity_proof =
            target->reported_alias_count > 1 ? DARTPLANT_CODE_IDENTITY_SHARED : identity_proof;
        targets_[entry] = target;
        return target;
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        targets_.clear();
        payloads_.clear();
    }

private:
    std::shared_ptr<DartCodePayload> FindOrCreatePayloadLocked(uintptr_t payload_start,
                                                               uint32_t instructions_length,
                                                               uint64_t code_object,
                                                               bool exact_identity) {
        for (auto payload_it = payloads_.begin(); payload_it != payloads_.end();) {
            auto payload = payload_it->second.lock();
            if (payload == nullptr) {
                payload_it = payloads_.erase(payload_it);
                continue;
            }
            if (payload->start == payload_start) {
                if (payload->instructions_length != instructions_length) return nullptr;
                std::lock_guard payload_lock(payload->mutex);
                if (payload->code_object != 0 && code_object != 0 &&
                    payload->code_object != code_object) {
                    return nullptr;
                }
                if (payload->code_object == 0) payload->code_object = code_object;
                if (exact_identity) payload->exact_identity = true;
                return payload;
            }
            ++payload_it;
        }
        auto payload = std::make_shared<DartCodePayload>();
        payload->id = payload_start;
        payload->start = payload_start;
        payload->instructions_length = instructions_length;
        payload->code_object = code_object;
        payload->exact_identity = exact_identity;
        payloads_[payload_start] = payload;
        return payload;
    }

    std::mutex mutex_;
    std::unordered_map<uintptr_t, std::weak_ptr<DartEntryTarget>> targets_;
    std::unordered_map<uintptr_t, std::weak_ptr<DartCodePayload>> payloads_;
};

}  // namespace dartplant

#endif  // DARTPLANT_CORE_METHOD_MODEL_H_
