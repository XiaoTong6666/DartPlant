// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/abi/probe.h"

#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

namespace dartplant::vm_abi {
namespace {

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

bool IsHeapObject(const RuntimeProfileRecord& profile, uint64_t tagged) {
    return (tagged & profile.raw_object.smi_tag_mask) == profile.raw_object.heap_object_tag &&
           tagged >= profile.raw_object.heap_object_tag;
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
    const uintptr_t object = static_cast<uintptr_t>(tagged - profile.raw_object.heap_object_tag);
    if (!ReadSelf(object, &tags)) return false;
    const uint64_t mask = (uint64_t{1} << profile.raw_object.class_id_tag_bits) - 1;
    *output = static_cast<uint32_t>((tags >> profile.raw_object.class_id_tag_shift) & mask);
    return true;
}

const ModuleImage* FindUniqueExecutableModule(const std::vector<ModuleImage>& modules,
                                              uintptr_t address) {
    const ModuleImage* match = nullptr;
    for (const ModuleImage& module : modules) {
        if (!module.ContainsExecutable(address, sizeof(uint32_t))) continue;
        if (match != nullptr) return nullptr;
        match = &module;
    }
    return match;
}

bool ProbeSafepoint(const RuntimeProfileRecord& profile, uint64_t thread, uint64_t heap_base,
                    uint32_t thread_offset, const std::vector<ModuleImage>& modules,
                    uint64_t* output_code, uint64_t* output_entry,
                    const ModuleImage** output_module) {
    if (output_code == nullptr || output_entry == nullptr || output_module == nullptr) {
        return false;
    }
    uint64_t tagged_code = 0;
    if (!ReadSelf(static_cast<uintptr_t>(thread) + thread_offset, &tagged_code) ||
        !TaggedInHeapWindow(profile, heap_base, tagged_code)) {
        return false;
    }
    uint32_t cid = 0;
    if (!ReadCid(profile, tagged_code, &cid) || cid != profile.live_vm.cid_code) return false;
    const uintptr_t code = static_cast<uintptr_t>(tagged_code - profile.raw_object.heap_object_tag);
    uint64_t entry = 0;
    if (!ReadSelf(code + profile.live_vm.code_entry_point_offset, &entry) || entry == 0) {
        return false;
    }
    const ModuleImage* module = FindUniqueExecutableModule(modules, static_cast<uintptr_t>(entry));
    if (module == nullptr) return false;
    *output_code = tagged_code;
    *output_entry = entry;
    *output_module = module;
    return true;
}

}  // namespace

const char* CandidateProbeStageName(const CandidateProbeStage& stage, const RootProof& roots) {
    if (stage == CandidateProbeStage::kRuntimeRoots && !roots.passed) {
        return RootProofStageName(roots.stage);
    }
    switch (stage) {
    case CandidateProbeStage::kNotStarted:
        return "not-started";
    case CandidateProbeStage::kRuntimeRoots:
        return "runtime-roots";
    case CandidateProbeStage::kEnterSafepoint:
        return "enter-safepoint";
    case CandidateProbeStage::kExitSafepoint:
        return "exit-safepoint";
    case CandidateProbeStage::kSafepointModuleRelation:
        return "safepoint-module-relation";
    case CandidateProbeStage::kComplete:
        return "complete";
    }
    return "unknown";
}

CandidateProbe ProbeCandidate(const CandidateProbeInput& input) {
    CandidateProbe probe{};
    if (input.roots.profile == nullptr || input.modules == nullptr) return probe;
    const RuntimeProfileRecord& profile = *input.roots.profile;

    probe.stage = CandidateProbeStage::kRuntimeRoots;
    probe.roots = ProveRuntimeRoots(input.roots);
    if (!probe.roots.passed) return probe;

    const ModuleImage* enter_module = nullptr;
    probe.stage = CandidateProbeStage::kEnterSafepoint;
    if (!ProbeSafepoint(profile, input.roots.thread, probe.roots.heap_base,
                        profile.thread_bridge.enter_safepoint_stub_offset, *input.modules,
                        &probe.enter_code, &probe.enter_entry, &enter_module)) {
        return probe;
    }

    const ModuleImage* exit_module = nullptr;
    probe.stage = CandidateProbeStage::kExitSafepoint;
    if (!ProbeSafepoint(profile, input.roots.thread, probe.roots.heap_base,
                        profile.thread_bridge.exit_safepoint_stub_offset, *input.modules,
                        &probe.exit_code, &probe.exit_entry, &exit_module)) {
        return probe;
    }

    probe.stage = CandidateProbeStage::kSafepointModuleRelation;
    if (enter_module != exit_module) return probe;

    probe.stage = CandidateProbeStage::kComplete;
    probe.code_module = enter_module;
    probe.passed = true;
    return probe;
}

}  // namespace dartplant::vm_abi
