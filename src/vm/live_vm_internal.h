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
    uint64_t runtime_image_incarnation_epoch = 0;
    uint64_t engine_incarnation_epoch = 0;
    uint64_t isolate_group_incarnation_epoch = 0;
    uint64_t runtime_generation = 0;
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
                                              const RuntimeProfileRecord& live_index_profile,
                                              const RuntimeProfileRecord* deferred_profile,
                                              DartPlantLiveVmFunctionVisitor visitor,
                                              void* user_data,
                                              DartPlantLiveVmFunctionIndexInfo* out_info);

// Candidate-scoped canonical Bool proof used before capability selection has
// chosen a whole source row. Unlike ResolveLiveVmCanonicalBoolRoots(), this
// deliberately does not compare context.profile_version; the candidate's own
// raw-object/CID/Bool layout is used for the complete proof.
DartPlantStatus ProbeLiveVmCanonicalBoolRootsForCandidate(const DartPlantLiveVmContext& context,
                                                          const RuntimeProfileRecord& candidate,
                                                          uint64_t* out_true, uint64_t* out_false);

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
//
// The current-profile helper preserves the historical exact-row contract used
// after a LiveVmContext has already been bound to one source row. The candidate
// probe deliberately does not require context.profile_version equality: it is
// used while capability-scoped ABI selection is still comparing multiple rows
// that share the same already-proven core ABI.
DartPlantStatus ReadLiveVmRootProgramHashForCurrentProfile(const DartPlantLiveVmContext& context,
                                                           const RuntimeProfileRecord& profile,
                                                           uint32_t* out_program_hash);
DartPlantStatus ProbeLiveVmRootProgramHashForCandidate(const DartPlantLiveVmContext& context,
                                                       const RuntimeProfileRecord& candidate,
                                                       uint32_t* out_program_hash);

}  // namespace dartplant

#endif  // DARTPLANT_VM_LIVE_VM_INTERNAL_H_
