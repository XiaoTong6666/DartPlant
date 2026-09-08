// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/abi/candidate_set.h"

#include <set>

namespace dartplant::vm_abi {

const char* AbiDomainName(AbiDomain domain) {
    switch (domain) {
    case AbiDomain::kCore:
        return "core";
    case AbiDomain::kCall:
        return "call";
    case AbiDomain::kObject:
        return "object";
    case AbiDomain::kTransition:
        return "transition";
    case AbiDomain::kException:
        return "exception";
    }
    return "unknown";
}

std::string_view DomainAbiId(const RuntimeProfileRecord& profile, AbiDomain domain) {
    switch (domain) {
    case AbiDomain::kCore:
        return profile.abi_identity.core == nullptr ? std::string_view{}
                                                    : profile.abi_identity.core;
    case AbiDomain::kCall:
        return profile.abi_identity.call == nullptr ? std::string_view{}
                                                    : profile.abi_identity.call;
    case AbiDomain::kObject:
        return profile.abi_identity.object == nullptr ? std::string_view{}
                                                      : profile.abi_identity.object;
    case AbiDomain::kTransition:
        return profile.abi_identity.transition == nullptr ? std::string_view{}
                                                          : profile.abi_identity.transition;
    case AbiDomain::kException:
        return profile.abi_identity.exception == nullptr ? std::string_view{}
                                                         : profile.abi_identity.exception;
    }
    return {};
}

std::string BuildDomainAbiKey(const RuntimeProfileRecord& profile, AbiDomainMask domains) {
    std::string key;
    for (AbiDomain domain : {AbiDomain::kCore, AbiDomain::kCall, AbiDomain::kObject,
                             AbiDomain::kTransition, AbiDomain::kException}) {
        if ((domains & AbiDomainBit(domain)) == 0) continue;
        const std::string_view id = DomainAbiId(profile, domain);
        if (id.empty()) return {};
        key.append(id);
        key.push_back('\x1f');
    }
    return key;
}

DomainSelection SelectDomainAbi(const AbiCandidateSet& candidates, AbiDomain domain,
                                const std::vector<bool>& compatible) {
    DomainSelection selection{};
    if (compatible.size() != candidates.profiles.size()) return selection;
    std::set<std::string_view> abi_ids;
    for (size_t index = 0; index < candidates.profiles.size(); ++index) {
        const RuntimeProfileRecord* profile = candidates.profiles[index];
        if (!compatible[index] || profile == nullptr) continue;
        const std::string_view abi_id = DomainAbiId(*profile, domain);
        if (abi_id.empty()) continue;
        ++selection.compatible_rows;
        abi_ids.insert(abi_id);
    }
    selection.distinct_abis = abi_ids.size();
    if (abi_ids.size() != 1) return selection;
    selection.abi_id = *abi_ids.begin();
    for (size_t index = 0; index < candidates.profiles.size(); ++index) {
        const RuntimeProfileRecord* profile = candidates.profiles[index];
        if (!compatible[index] || profile == nullptr ||
            DomainAbiId(*profile, domain) != selection.abi_id) {
            continue;
        }
        if (selection.representative == nullptr || profile == candidates.representative) {
            selection.representative = profile;
        }
    }
    return selection;
}

}  // namespace dartplant::vm_abi
