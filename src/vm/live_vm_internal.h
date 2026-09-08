#ifndef DARTPLANT_VM_LIVE_VM_INTERNAL_H_
#define DARTPLANT_VM_LIVE_VM_INTERNAL_H_

#include <cstdint>

#include "dartplant/advanced/live_vm.h"
#include "dartplant/invocation.h"
#include "vm/abi/probe.h"
#include "vm/runtime_profiles.h"

namespace dartplant {

struct LiveVmCandidateResolution {
    const RuntimeProfileRecord* profile = nullptr;
    vm_abi::CandidateProbe probe{};
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

DartPlantStatus ReadLiveVmFunctionSignatureForProfile(
    const DartPlantLiveVmContext& context, const RuntimeProfileRecord& profile, uint64_t function,
    DartPlantDartFunctionSignatureInfo* out_signature);
DartPlantStatus ReadLiveVmFunctionParameterForProfile(const DartPlantLiveVmContext& context,
                                                      const RuntimeProfileRecord& profile,
                                                      uint64_t function, uint32_t index,
                                                      DartPlantDartParameterInfo* out_parameter);

}  // namespace dartplant

#endif  // DARTPLANT_VM_LIVE_VM_INTERNAL_H_
