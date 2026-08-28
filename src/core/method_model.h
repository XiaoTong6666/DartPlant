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

struct DartCodeTarget {
    using Id = uintptr_t;

    mutable std::mutex mutex;
    Id id = 0;
    uintptr_t entry = 0;
    uint32_t code_size = 0;
    uint64_t code_object = 0;
    uint32_t reported_alias_count = 1;
    DartPlantCodeIdentityProof identity_proof = DARTPLANT_CODE_IDENTITY_UNKNOWN;
    std::vector<DartMethodIdentity> aliases;
    DartPlantHook* hook_record = nullptr;

    void Update(uint32_t new_code_size, uint64_t new_code_object, uint32_t alias_count,
                DartPlantCodeIdentityProof new_identity_proof = DARTPLANT_CODE_IDENTITY_UNKNOWN) {
        std::lock_guard lock(mutex);
        if (new_code_size != 0) code_size = new_code_size;
        if (new_code_object != 0) code_object = new_code_object;
        if (alias_count > reported_alias_count) reported_alias_count = alias_count;
        if (new_identity_proof == DARTPLANT_CODE_IDENTITY_SHARED || reported_alias_count > 1) {
            identity_proof = DARTPLANT_CODE_IDENTITY_SHARED;
        } else if (new_identity_proof == DARTPLANT_CODE_IDENTITY_UNIQUE &&
                   identity_proof == DARTPLANT_CODE_IDENTITY_UNKNOWN) {
            identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
        }
    }

    void AddAlias(const DartMethodIdentity& identity) {
        std::lock_guard lock(mutex);
        const auto found = std::find(aliases.begin(), aliases.end(), identity);
        if (found == aliases.end()) aliases.push_back(identity);
        if (aliases.size() > 1) {
            reported_alias_count =
                std::max<uint32_t>(reported_alias_count, static_cast<uint32_t>(aliases.size()));
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

    bool IsShared() const {
        std::lock_guard lock(mutex);
        return identity_proof == DARTPLANT_CODE_IDENTITY_SHARED || reported_alias_count > 1 ||
               aliases.size() > 1;
    }

    bool HasProvenUniqueIdentity() const {
        std::lock_guard lock(mutex);
        return identity_proof == DARTPLANT_CODE_IDENTITY_UNIQUE && reported_alias_count <= 1 &&
               aliases.size() <= 1;
    }
};

struct DartFunctionHandle {
    DartMethodIdentity identity;
    uint64_t function_object = 0;
    uint64_t code_object = 0;
    DartFunctionSource source = DartFunctionSource::kLegacyMetadata;
    std::shared_ptr<DartCodeTarget> code_target;

    DartCodeTarget::Id CodeTargetId() const { return code_target == nullptr ? 0 : code_target->id; }
};

class DartCodeTargetRegistry final {
public:
    std::shared_ptr<DartCodeTarget> GetOrCreate(
        uintptr_t entry, uint32_t code_size, uint64_t code_object = 0,
        uint32_t reported_alias_count = 1,
        DartPlantCodeIdentityProof identity_proof = DARTPLANT_CODE_IDENTITY_UNKNOWN) {
        if (entry == 0) return nullptr;
        std::lock_guard lock(mutex_);
        auto found = targets_.find(entry);
        if (found != targets_.end()) {
            if (auto existing = found->second.lock(); existing != nullptr) {
                existing->Update(code_size, code_object, reported_alias_count, identity_proof);
                return existing;
            }
        }
        auto target = std::make_shared<DartCodeTarget>();
        target->id = entry;
        target->entry = entry;
        target->code_size = code_size;
        target->code_object = code_object;
        target->reported_alias_count = std::max<uint32_t>(1, reported_alias_count);
        target->identity_proof =
            target->reported_alias_count > 1 ? DARTPLANT_CODE_IDENTITY_SHARED : identity_proof;
        targets_[entry] = target;
        return target;
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        targets_.clear();
    }

private:
    std::mutex mutex_;
    std::unordered_map<uintptr_t, std::weak_ptr<DartCodeTarget>> targets_;
};

}  // namespace dartplant

#endif  // DARTPLANT_CORE_METHOD_MODEL_H_
