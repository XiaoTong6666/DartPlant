#ifndef DARTPLANT_VM_LIVE_VM_INTERNAL_H_
#define DARTPLANT_VM_LIVE_VM_INTERNAL_H_

#include <cstdint>
#include <span>

#include "dartplant/advanced/live_vm.h"
#include "dartplant/invocation.h"
#include "vm/abi/probe.h"
#include "vm/runtime_profiles.h"

namespace dartplant {

struct LiveVmCandidateResolution {
    const RuntimeProfileRecord* profile = nullptr;
    vm_abi::CandidateProbe probe{};
};

struct LiveVmInstructionImage {
    uint64_t runtime_image_id = 0;
    uint32_t loading_unit_id = 0;
    DartPlantFlutterSnapshotInfo snapshot{};
};

DartPlantStatus ResolveLiveVmCandidateForArm64Context(const DartPlantFlutterSnapshotInfo& snapshot,
                                                      const DartPlantArm64Context& context,
                                                      LiveVmCandidateResolution* out_resolution);
DartPlantStatus ResolveLiveVmCandidateForRegisters(const DartPlantFlutterSnapshotInfo& snapshot,
                                                   const DartPlantLiveVmArm64Registers& registers,
                                                   LiveVmCandidateResolution* out_resolution,
                                                   DartPlantLiveVmContext* out_context);
DartPlantStatus VisitLiveVmFunctionsForProfile(const DartPlantLiveVmContext& context,
                                               const DartPlantFlutterSnapshotInfo& snapshot,
                                               const RuntimeProfileRecord& profile,
                                               DartPlantLiveVmFunctionVisitor visitor,
                                               void* user_data,
                                               DartPlantLiveVmFunctionIndexInfo* out_info);
// Runtime-only multi-image enumeration. Every physical Code entry must resolve
// into exactly one instruction image; records carry that image id so later
// method resolution never assumes the root isolate-instructions namespace.
DartPlantStatus VisitLiveVmFunctionsForImages(const DartPlantLiveVmContext& context,
                                              std::span<const LiveVmInstructionImage> images,
                                              const RuntimeProfileRecord& profile,
                                              DartPlantLiveVmFunctionVisitor visitor,
                                              void* user_data,
                                              DartPlantLiveVmFunctionIndexInfo* out_info);

DartPlantStatus ReadLiveVmFunctionSignatureForProfile(
    const DartPlantLiveVmContext& context, const RuntimeProfileRecord& profile, uint64_t function,
    DartPlantDartFunctionSignatureInfo* out_signature);
DartPlantStatus ReadLiveVmFunctionParameterForProfile(const DartPlantLiveVmContext& context,
                                                      const RuntimeProfileRecord& profile,
                                                      uint64_t function, uint32_t index,
                                                      DartPlantDartParameterInfo* out_parameter);

// Reads ObjectStore.loading_units[0], the root ProgramVisitor::Hash() Smi used
// by Dart's deferred snapshot loader to reject units from another program.
// This is intentionally a live-VM relational proof: secondary ELF files carry
// the same uint32_t in their serialized snapshot payload, while the root ELF
// does not expose it at a stable artifact offset.
DartPlantStatus ReadLiveVmRootProgramHashForProfile(const DartPlantLiveVmContext& context,
                                                    const RuntimeProfileRecord& profile,
                                                    uint32_t* out_program_hash);

}  // namespace dartplant

#endif  // DARTPLANT_VM_LIVE_VM_INTERNAL_H_
