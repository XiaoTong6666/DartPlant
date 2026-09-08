// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_VM_ABI_CANDIDATE_SET_H_
#define DARTPLANT_VM_ABI_CANDIDATE_SET_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vm/runtime_profiles.h"

namespace dartplant::vm_abi {

enum class AbiDomain : uint8_t {
    kCore = 0,
    kCall,
    kObject,
    kTransition,
    kException,
};

using AbiDomainMask = uint32_t;

constexpr AbiDomainMask AbiDomainBit(AbiDomain domain) {
    return AbiDomainMask{1} << static_cast<uint8_t>(domain);
}

constexpr AbiDomainMask operator|(AbiDomain left, AbiDomain right) {
    return AbiDomainBit(left) | AbiDomainBit(right);
}

constexpr AbiDomainMask operator|(AbiDomainMask left, AbiDomain right) {
    return left | AbiDomainBit(right);
}

const char* AbiDomainName(AbiDomain domain);
std::string_view DomainAbiId(const RuntimeProfileRecord& profile, AbiDomain domain);
std::string BuildDomainAbiKey(const RuntimeProfileRecord& profile, AbiDomainMask domains);

struct AbiCandidateSet {
    std::vector<const RuntimeProfileRecord*> profiles;
    const RuntimeProfileRecord* representative = nullptr;
    std::string_view core_abi_id;

    bool empty() const { return profiles.empty(); }
};

struct DomainSelection {
    const RuntimeProfileRecord* representative = nullptr;
    std::string_view abi_id;
    size_t compatible_rows = 0;
    size_t distinct_abis = 0;

    bool passed() const { return representative != nullptr && distinct_abis == 1; }
    bool ambiguous() const { return distinct_abis > 1; }
};

DomainSelection SelectDomainAbi(const AbiCandidateSet& candidates, AbiDomain domain,
                                const std::vector<bool>& compatible);

}  // namespace dartplant::vm_abi

#endif  // DARTPLANT_VM_ABI_CANDIDATE_SET_H_
