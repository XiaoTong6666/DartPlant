// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>

#include "abi/calling_convention.h"
#include "abi/evidence_solver.h"
#include "runtime/runtime_internal.h"

namespace dartplant {
namespace {

std::optional<abi::DartAbiRepresentation> ToInternalRepresentation(
    DartPlantAbiRepresentation representation, bool is_result) {
    switch (representation) {
    case DARTPLANT_ABI_REPRESENTATION_UNKNOWN:
        return abi::DartAbiRepresentation::kUnknown;
    case DARTPLANT_ABI_REPRESENTATION_TAGGED:
        return abi::DartAbiRepresentation::kTagged;
    case DARTPLANT_ABI_REPRESENTATION_UNBOXED_INT64:
        return abi::DartAbiRepresentation::kUnboxedInt64;
    case DARTPLANT_ABI_REPRESENTATION_UNBOXED_DOUBLE:
        return abi::DartAbiRepresentation::kUnboxedDouble;
    case DARTPLANT_ABI_REPRESENTATION_PAIR_OF_TAGGED:
        return is_result ? std::optional(abi::DartAbiRepresentation::kPairOfTagged) : std::nullopt;
    }
    return std::nullopt;
}

abi::DartAbiSlotEvidence CompilerSlot(abi::DartAbiRepresentation representation) {
    return {
        .representation = representation,
        .source = abi::DartAbiEvidenceSource::kCompilerOracle,
        .proof = representation == abi::DartAbiRepresentation::kUnknown
                     ? abi::DartAbiProofState::kUnknown
                     : abi::DartAbiProofState::kProven,
    };
}

bool SameEvidenceTarget(const RuntimeAbiEvidenceEntry& entry, const DartPlantMethod* method,
                        uint64_t generation) {
    return method != nullptr && method->function != nullptr &&
           entry.identity == method->function->identity &&
           entry.code_target == MethodTarget(method) && entry.generation == generation;
}

DartPlantMethodAbiState PublicAbiState(const RuntimeAbiEvidenceEntry& entry) {
    if (entry.call_layout != nullptr && entry.layout_status == abi::DartCallLayoutStatus::kOk) {
        return DARTPLANT_METHOD_ABI_VERIFIED;
    }
    if (entry.resolution.conflicting ||
        entry.layout_status == abi::DartCallLayoutStatus::kConflictingEvidence) {
        return DARTPLANT_METHOD_ABI_CONFLICTING;
    }
    switch (entry.layout_status) {
    case abi::DartCallLayoutStatus::kIncompleteEvidence:
        return DARTPLANT_METHOD_ABI_INCOMPLETE;
    case abi::DartCallLayoutStatus::kUnsupportedRepresentation:
    case abi::DartCallLayoutStatus::kOptionalArgumentsUnsupported:
    case abi::DartCallLayoutStatus::kInvalidCallingConvention:
        return DARTPLANT_METHOD_ABI_UNSUPPORTED;
    case abi::DartCallLayoutStatus::kOk:
        break;
    case abi::DartCallLayoutStatus::kConflictingEvidence:
        return DARTPLANT_METHOD_ABI_CONFLICTING;
    }
    return DARTPLANT_METHOD_ABI_INCOMPLETE;
}

}  // namespace

std::shared_ptr<const abi::DartCallLayout> FindRuntimeCallLayoutLocked(
    const DartPlantRuntime* runtime, const DartPlantMethod* method) {
    if (runtime == nullptr || method == nullptr || method->function == nullptr ||
        method->function->code_target == nullptr ||
        !method->function->code_target->HasProvenUniqueIdentity()) {
        return nullptr;
    }
    const uint64_t generation = runtime->generation->load(std::memory_order_acquire);
    const auto found = std::find_if(runtime->abi_evidence.begin(), runtime->abi_evidence.end(),
                                    [method, generation](const auto& entry) {
                                        return SameEvidenceTarget(entry, method, generation);
                                    });
    return found == runtime->abi_evidence.end() ? nullptr : found->call_layout;
}

}  // namespace dartplant

extern "C" DartPlantStatus dartplant_runtime_register_compiler_abi_evidence(
    DartPlantRuntime* runtime, const DartPlantMethod* method,
    const DartPlantCompilerAbiEvidence* evidence) {
    constexpr size_t kCompilerAbiEvidenceV1Size =
        offsetof(DartPlantCompilerAbiEvidence, library_uri);
    if (runtime == nullptr || method == nullptr || evidence == nullptr ||
        evidence->struct_size < kCompilerAbiEvidenceV1Size || evidence->snapshot_hash == nullptr ||
        evidence->snapshot_hash[0] == '\0' || evidence->app_build_id == nullptr ||
        evidence->app_build_id[0] == '\0' || evidence->code_fingerprint == nullptr ||
        evidence->code_fingerprint[0] == '\0' ||
        (evidence->parameter_count != 0 && evidence->parameter_representations == nullptr)) {
        dartplant::SetLastError("compiler ABI evidence arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (evidence->must_use_stack_calling_convention > 1 || evidence->has_optional_parameters > 1 ||
        evidence->has_overrides_with_less_direct_parameters > 1) {
        dartplant::SetLastError("compiler ABI evidence boolean flags must be zero or one");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    if (runtime->state != DARTPLANT_RUNTIME_READY ||
        !dartplant::IsCurrentRuntimeMethod(runtime, method)) {
        dartplant::SetLastError("compiler ABI evidence targets a stale or unready runtime method");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (method->record.entry_kind != DARTPLANT_ENTRY_DEFAULT) {
        dartplant::SetLastError(
            "typed ABI evidence currently supports only the default Dart entry");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (method->function == nullptr || method->function->code_target == nullptr) {
        dartplant::SetLastError("compiler ABI evidence method has no CodeTarget");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (method->function->code_target->IsShared()) {
        dartplant::SetLastError(
            "compiler ABI evidence cannot create a typed frame for an identity-ambiguous shared CodeTarget");
        return DARTPLANT_SHARED_CODE_ENTRY;
    }
    if (!runtime->snapshot.has_value() || !runtime->selected_app_module.has_value()) {
        dartplant::SetLastError("compiler ABI evidence cannot bind without the current AOT image");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (runtime->snapshot->snapshot_hash != evidence->snapshot_hash) {
        dartplant::SetLastError(
            "compiler ABI evidence snapshot hash does not match the live runtime");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!dartplant::EqualsIgnoreCaseAscii(runtime->selected_app_module->build_id,
                                          evidence->app_build_id)) {
        dartplant::SetLastError(
            "compiler ABI evidence app build-id does not match the live module");
        return DARTPLANT_BUILD_ID_MISMATCH;
    }
    const uint32_t code_size = dartplant::MethodCodeSize(method);
    const uintptr_t target = dartplant::MethodTarget(method);
    if (target == 0 || code_size == 0 ||
        dartplant::FingerprintCode(reinterpret_cast<const void*>(target), code_size) !=
            evidence->code_fingerprint) {
        dartplant::SetLastError(
            "compiler ABI evidence code fingerprint does not match the CodeTarget");
        return DARTPLANT_FINGERPRINT_MISMATCH;
    }
    const bool has_function_binding = evidence->struct_size >= sizeof(*evidence);
    if (method->function->source == dartplant::DartFunctionSource::kOfflineSnapshotIndex &&
        !has_function_binding) {
        dartplant::SetLastError(
            "artifact compiler ABI evidence requires an exact Function identity/address binding");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (has_function_binding) {
        const std::string_view evidence_library =
            evidence->library_uri == nullptr ? "" : evidence->library_uri;
        const std::string_view evidence_class =
            evidence->class_name == nullptr ? "" : evidence->class_name;
        const std::string_view evidence_function =
            evidence->function_name == nullptr ? "" : evidence->function_name;
        if (evidence_library != method->function->identity.library_uri ||
            evidence_class != method->function->identity.class_name ||
            evidence_function != method->function->identity.function_name ||
            evidence->entry_kind != method->function->identity.entry_kind ||
            evidence->entry_va == 0 || evidence->code_size != code_size) {
            dartplant::SetLastError(
                "compiler ABI evidence Function identity does not match the runtime method");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        const auto evidence_target = runtime->snapshot->ResolveInstructionVa(
            *runtime->selected_app_module, evidence->entry_va);
        if (!evidence_target.has_value() || *evidence_target != target) {
            dartplant::SetLastError(
                "compiler ABI evidence entry VA does not resolve to the runtime CodeTarget");
            return DARTPLANT_PROFILE_MISMATCH;
        }
    }
    if (evidence->must_use_stack_calling_convention != 0 &&
        evidence->max_parameters_in_registers != 0) {
        dartplant::SetLastError(
            "compiler ABI evidence cannot combine forced stack calling convention with register parameters");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (evidence->max_parameters_in_registers > evidence->parameter_count) {
        dartplant::SetLastError(
            "compiler ABI evidence register parameter limit exceeds fixed formals");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (method->function->code_target->HookRecord() != nullptr) {
        dartplant::SetLastError(
            "compiler ABI evidence must be registered before installing the hook");
        return DARTPLANT_ALREADY_HOOKED;
    }

    // A retained live Function gives us an independent FunctionType source for
    // cardinality validation. PRODUCT AOT may deliberately drop the Function
    // object while keeping its Code; artifact-index methods therefore have no
    // FunctionType to consult. In that case the exact compiler evidence itself
    // is the only surviving formal-slot source, and it is accepted only after
    // the snapshot hash, app build-id and final Code fingerprint checks above.
    if (method->function->source == dartplant::DartFunctionSource::kLiveVm) {
        if (!runtime->live_vm_context.has_value() || !runtime->snapshot.has_value() ||
            method->function->function_object == 0) {
            dartplant::SetLastError("live FunctionType is unavailable for ABI evidence validation");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        DartPlantFlutterSnapshotInfo snapshot_info{};
        snapshot_info.struct_size = sizeof(snapshot_info);
        dartplant::FillSnapshotInfo(*runtime->snapshot, &snapshot_info);
        DartPlantDartFunctionSignatureInfo signature{};
        signature.struct_size = sizeof(signature);
        const DartPlantStatus signature_status = dartplant_live_vm_read_function_signature(
            &*runtime->live_vm_context, &snapshot_info, method->function->function_object,
            &signature);
        if (signature_status != DARTPLANT_OK) return signature_status;
        if (signature.fixed_parameter_count != evidence->parameter_count) {
            dartplant::SetLastError(
                "compiler ABI evidence fixed-parameter count does not match FunctionType");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        const bool signature_has_optional = signature.optional_parameter_count != 0;
        if (signature_has_optional != (evidence->has_optional_parameters != 0)) {
            dartplant::SetLastError(
                "compiler ABI evidence optional-parameter flag does not match FunctionType");
            return DARTPLANT_PROFILE_MISMATCH;
        }
    } else if (method->function->source != dartplant::DartFunctionSource::kOfflineSnapshotIndex) {
        dartplant::SetLastError(
            "compiler ABI evidence requires a live Function or exact artifact-index method");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    dartplant::abi::DartFunctionAbiEvidence provider;
    provider.parameters.reserve(evidence->parameter_count);
    for (uint32_t index = 0; index < evidence->parameter_count; ++index) {
        const auto representation =
            dartplant::ToInternalRepresentation(evidence->parameter_representations[index], false);
        if (!representation.has_value()) {
            dartplant::SetLastError(
                "compiler ABI evidence contains an invalid parameter representation");
            return DARTPLANT_INVALID_ARGUMENT;
        }
        provider.parameters.push_back(dartplant::CompilerSlot(*representation));
    }
    const auto result_representation =
        dartplant::ToInternalRepresentation(evidence->result_representation, true);
    if (!result_representation.has_value()) {
        dartplant::SetLastError("compiler ABI evidence contains an invalid result representation");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    provider.result = dartplant::CompilerSlot(*result_representation);
    provider.has_stack_calling_convention = true;
    provider.must_use_stack_calling_convention = evidence->must_use_stack_calling_convention != 0;
    provider.has_overrides_with_less_direct_parameters =
        evidence->has_overrides_with_less_direct_parameters != 0;
    provider.has_optional_parameter_info = true;
    provider.has_optional_parameters = evidence->has_optional_parameters != 0;
    provider.has_max_parameters_in_registers = true;
    provider.max_parameters_in_registers = evidence->max_parameters_in_registers;

    const uint64_t generation = runtime->generation->load(std::memory_order_acquire);
    auto existing =
        std::find_if(runtime->abi_evidence.begin(), runtime->abi_evidence.end(),
                     [method, generation](const auto& candidate) {
                         return dartplant::SameEvidenceTarget(candidate, method, generation);
                     });
    if (existing == runtime->abi_evidence.end()) {
        dartplant::RuntimeAbiEvidenceEntry entry;
        entry.identity = method->function->identity;
        entry.code_target = dartplant::MethodTarget(method);
        entry.generation = generation;
        entry.formal_parameter_count = evidence->parameter_count;
        runtime->abi_evidence.push_back(std::move(entry));
        existing = std::prev(runtime->abi_evidence.end());
    } else if (existing->formal_parameter_count != evidence->parameter_count) {
        existing->resolution.conflicting = true;
        existing->layout_status = dartplant::abi::DartCallLayoutStatus::kConflictingEvidence;
        existing->call_layout.reset();
        dartplant::SetLastError(
            "compiler ABI evidence changes the established formal parameter count");
        return DARTPLANT_UNSUPPORTED_ABI;
    }

    existing->providers.push_back(std::move(provider));
    existing->resolution = dartplant::abi::ResolveFunctionAbiEvidence(
        existing->providers, existing->formal_parameter_count);
    auto layout = std::make_shared<dartplant::abi::DartCallLayout>();
    existing->layout_status = dartplant::abi::ComputeDartCallLayout(
        existing->resolution, dartplant::abi::Arm64AotCallingConventionProfile(), layout.get());
    existing->call_layout = existing->layout_status == dartplant::abi::DartCallLayoutStatus::kOk &&
                                    method->function->code_target->HasProvenUniqueIdentity()
                                ? layout
                                : nullptr;

    if (existing->layout_status == dartplant::abi::DartCallLayoutStatus::kConflictingEvidence ||
        existing->resolution.conflicting) {
        dartplant::SetLastError(
            "compiler ABI evidence conflicts with previously registered exact evidence");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (existing->layout_status != dartplant::abi::DartCallLayoutStatus::kOk) {
        dartplant::SetLastError(
            "compiler ABI evidence was retained but is insufficient for a verified DartCallLayout");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_runtime_get_method_abi_info(const DartPlantRuntime* runtime,
                                                                 const DartPlantMethod* method,
                                                                 DartPlantMethodAbiInfo* out_info) {
    if (runtime == nullptr || method == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(DartPlantMethodAbiInfo)) {
        dartplant::SetLastError("method ABI info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    if (runtime->state != DARTPLANT_RUNTIME_READY ||
        !dartplant::IsCurrentRuntimeMethod(runtime, method)) {
        dartplant::SetLastError("method ABI info targets a stale or unready runtime method");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    DartPlantMethodAbiInfo info{};
    info.struct_size = sizeof(info);
    const uint64_t generation = runtime->generation->load(std::memory_order_acquire);
    const auto found =
        std::find_if(runtime->abi_evidence.begin(), runtime->abi_evidence.end(),
                     [method, generation](const auto& entry) {
                         return dartplant::SameEvidenceTarget(entry, method, generation);
                     });
    if (found == runtime->abi_evidence.end()) {
        info.state = DARTPLANT_METHOD_ABI_NONE;
    } else {
        info.parameter_count = static_cast<uint32_t>(found->resolution.parameters.size());
        if (method->function != nullptr && method->function->code_target != nullptr &&
            method->function->code_target->IsShared()) {
            info.state = DARTPLANT_METHOD_ABI_UNSUPPORTED;
        } else if (method->function != nullptr && method->function->code_target != nullptr &&
                   !method->function->code_target->HasProvenUniqueIdentity()) {
            info.state = DARTPLANT_METHOD_ABI_INCOMPLETE;
        } else {
            info.state = dartplant::PublicAbiState(*found);
            info.has_verified_call_layout = found->call_layout != nullptr ? 1 : 0;
            info.stack_words = found->call_layout == nullptr ? 0 : found->call_layout->stack_words;
        }
    }
    *out_info = info;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}
