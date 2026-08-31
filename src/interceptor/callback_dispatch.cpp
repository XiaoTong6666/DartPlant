// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstring>
#include <new>

#if defined(DARTPLANT_USE_PTHREAD_TLS)
#include <pthread.h>
#endif

#include "runtime/runtime_internal.h"

namespace {

[[noreturn]] void FatalGeneratedBridgeFailure(const char* message) {
    dartplant::SetLastError(message);
    __builtin_trap();
}

constexpr uint32_t kMaxInvocationDepth = 64;
constexpr uint32_t kMaxGeneratedRootBindings = 96;
enum class GeneratedRootRole : uint8_t {
    kPadding = 0,
    kInput,
    kResult,
};

struct GeneratedRootBinding {
    dartplant::abi::DartAbiLocation location{};
    GeneratedRootRole role = GeneratedRootRole::kPadding;
};

struct DispatchFrame {
    DartPlantArm64Context context{};
    DartPlantInvocation invocation{};
    uintptr_t entry_spreg = 0;
    uintptr_t entry_caller_fp = 0;
    uintptr_t invoke_original_native_frame = 0;
    std::array<GeneratedRootBinding, kMaxGeneratedRootBindings> generated_roots{};
    std::array<uint64_t, kMaxGeneratedRootBindings> generated_root_values{};
    uint32_t generated_root_count = 0;
    void* generated_root_lease = nullptr;
    DartPlantGeneratedTransitionFrame generated_transition{};
};

struct DispatchStack {
    std::array<DispatchFrame, kMaxInvocationDepth> frames{};
    uint32_t depth = 0;
};

#if defined(DARTPLANT_USE_PTHREAD_TLS)
pthread_key_t g_dispatch_stack_key;
pthread_once_t g_dispatch_stack_key_once = PTHREAD_ONCE_INIT;
bool g_dispatch_stack_key_ready = false;

void DestroyDispatchStack(void* value) { delete static_cast<DispatchStack*>(value); }

void CreateDispatchStackKey() {
    if (pthread_key_create(&g_dispatch_stack_key, DestroyDispatchStack) == 0) {
        g_dispatch_stack_key_ready = true;
    }
}

DispatchStack* CurrentDispatchStack() {
    pthread_once(&g_dispatch_stack_key_once, CreateDispatchStackKey);
    if (!g_dispatch_stack_key_ready) return nullptr;
    auto* stack = static_cast<DispatchStack*>(pthread_getspecific(g_dispatch_stack_key));
    if (stack != nullptr) return stack;
    stack = new (std::nothrow) DispatchStack();
    if (stack == nullptr || pthread_setspecific(g_dispatch_stack_key, stack) != 0) {
        delete stack;
        return nullptr;
    }
    return stack;
}
#else
thread_local DispatchStack g_dispatch_stack;

DispatchStack* CurrentDispatchStack() { return &g_dispatch_stack; }
#endif

bool InvokeCallback(DartPlantInvocationCallback callback, DartPlantInvocation* invocation,
                    void* user_data) {
    if (callback == nullptr) return true;
    const DartPlantArm64Context saved_context = *invocation->context;
    const bool saved_skip_original = invocation->skip_original;
    const bool saved_call_original = invocation->call_original;
    try {
        callback(invocation, user_data);
        return true;
    } catch (...) {
        // Like Pine, discard state changed by a callback that exits unexpectedly.
        // A C ABI callback must never unwind through the native trampoline.
        *invocation->context = saved_context;
        invocation->skip_original = saved_skip_original;
        invocation->call_original = saved_call_original;
        dartplant::SetLastError("DartPlant callback threw across the C ABI");
        return false;
    }
}

DispatchFrame* CurrentFrame() {
    DispatchStack* stack = CurrentDispatchStack();
    return stack == nullptr || stack->depth == 0 ? nullptr : &stack->frames[stack->depth - 1];
}

bool ReadGeneratedRootLocation(const DispatchFrame& frame,
                               const dartplant::abi::DartAbiLocation& location,
                               uint64_t* out_value) {
    if (out_value == nullptr) return false;
    switch (location.kind) {
    case dartplant::abi::DartAbiLocationKind::kGpRegister:
        if (location.register_index >= 31) return false;
        *out_value = frame.context.x[location.register_index];
        return true;
    case dartplant::abi::DartAbiLocationKind::kEntryStack: {
        if (frame.invocation.call_layout == nullptr ||
            frame.invocation.call_layout->dart_sp_register >= 31 || location.stack_offset < 0) {
            return false;
        }
        const uintptr_t base = frame.context.x[frame.invocation.call_layout->dart_sp_register];
        if (base == 0 || static_cast<uint64_t>(location.stack_offset) > UINTPTR_MAX - base) {
            return false;
        }
        std::memcpy(out_value, reinterpret_cast<const void*>(base + location.stack_offset),
                    sizeof(*out_value));
        return true;
    }
    case dartplant::abi::DartAbiLocationKind::kFpuRegister:
    case dartplant::abi::DartAbiLocationKind::kUnknown:
        return false;
    }
    return false;
}

bool WriteGeneratedRootLocation(DispatchFrame* frame,
                                const dartplant::abi::DartAbiLocation& location, uint64_t value) {
    if (frame == nullptr) return false;
    switch (location.kind) {
    case dartplant::abi::DartAbiLocationKind::kGpRegister:
        if (location.register_index >= 31) return false;
        frame->context.x[location.register_index] = value;
        return true;
    case dartplant::abi::DartAbiLocationKind::kEntryStack: {
        if (frame->invocation.call_layout == nullptr ||
            frame->invocation.call_layout->dart_sp_register >= 31 || location.stack_offset < 0) {
            return false;
        }
        const uintptr_t base = frame->context.x[frame->invocation.call_layout->dart_sp_register];
        if (base == 0 || static_cast<uint64_t>(location.stack_offset) > UINTPTR_MAX - base) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(base + location.stack_offset), &value, sizeof(value));
        return true;
    }
    case dartplant::abi::DartAbiLocationKind::kFpuRegister:
    case dartplant::abi::DartAbiLocationKind::kUnknown:
        return false;
    }
    return false;
}

bool SameGeneratedRootBinding(const GeneratedRootBinding& binding,
                              const dartplant::abi::DartAbiLocation& location,
                              GeneratedRootRole role) {
    return binding.role == role && binding.location == location;
}

bool AddGeneratedRootBinding(DispatchFrame* frame, const dartplant::abi::DartAbiLocation& location,
                             GeneratedRootRole role) {
    if (frame == nullptr || role == GeneratedRootRole::kPadding ||
        (location.kind != dartplant::abi::DartAbiLocationKind::kGpRegister &&
         location.kind != dartplant::abi::DartAbiLocationKind::kEntryStack)) {
        return false;
    }
    for (uint32_t index = 0; index < frame->generated_root_count; ++index) {
        if (SameGeneratedRootBinding(frame->generated_roots[index], location, role)) return true;
    }
    if (frame->generated_root_count >= frame->generated_roots.size()) return false;
    frame->generated_roots[frame->generated_root_count++] = {
        .location = location,
        .role = role,
    };
    return true;
}

bool BuildGeneratedRootBindings(DispatchFrame* frame) {
    if (frame == nullptr || frame->invocation.call_layout == nullptr) return false;
    frame->generated_root_count = 0;
    const auto& layout = *frame->invocation.call_layout;
    const auto* parameters = InvocationParameters(&frame->invocation);
    if (parameters == nullptr) return false;
    for (const auto& parameter : *parameters) {
        if (parameter.representation != dartplant::abi::DartAbiRepresentation::kTagged) continue;
        if (parameter.location.count != 1 ||
            !AddGeneratedRootBinding(frame, parameter.location.locations[0],
                                     GeneratedRootRole::kInput)) {
            return false;
        }
    }
    if (layout.has_closure_receiver &&
        !AddGeneratedRootBinding(frame, layout.closure_receiver_location,
                                 GeneratedRootRole::kInput)) {
        return false;
    }
    if (layout.has_arguments_descriptor &&
        !AddGeneratedRootBinding(frame, layout.arguments_descriptor_location,
                                 GeneratedRootRole::kInput)) {
        return false;
    }
    if (layout.result.representation == dartplant::abi::DartAbiRepresentation::kTagged) {
        if (layout.result.location.count != 1 ||
            !AddGeneratedRootBinding(frame, layout.result.location.locations[0],
                                     GeneratedRootRole::kResult)) {
            return false;
        }
    } else if (layout.result.representation ==
               dartplant::abi::DartAbiRepresentation::kPairOfTagged) {
        if (layout.result.location.count != 2 ||
            !AddGeneratedRootBinding(frame, layout.result.location.locations[0],
                                     GeneratedRootRole::kResult) ||
            !AddGeneratedRootBinding(frame, layout.result.location.locations[1],
                                     GeneratedRootRole::kResult)) {
            return false;
        }
    }
    // A transition with no tagged formals/results still needs one opaque root
    // lease so the adapter can tie the transition lifetime to this invocation.
    if (frame->generated_root_count == 0) {
        frame->generated_roots[0] = {};
        frame->generated_root_count = 1;
    }
    frame->invocation.generated_root_accesses.clear();
    frame->invocation.generated_root_accesses.reserve(frame->generated_root_count);
    for (uint32_t index = 0; index < frame->generated_root_count; ++index) {
        if (frame->generated_roots[index].role == GeneratedRootRole::kPadding) continue;
        frame->invocation.generated_root_accesses.push_back({
            .location = frame->generated_roots[index].location,
            .root_index = index,
            .is_result = frame->generated_roots[index].role == GeneratedRootRole::kResult,
        });
    }
    frame->generated_transition = {
        .struct_size = sizeof(DartPlantGeneratedTransitionFrame),
        .flags = DARTPLANT_GENERATED_TRANSITION_SYNTHETIC_EXIT_FRAME,
        .thread = frame->context.x[26],
        .dart_sp = frame->context.x[15],
        .exit_frame = 0,
        .caller_fp = frame->context.x[29],
        .caller_lr = frame->context.x[30],
    };
    return true;
}

bool PrepareGeneratedExitFrame(DispatchFrame* frame) {
    if (frame == nullptr || frame->generated_transition.dart_sp < 4 * sizeof(uint64_t)) {
        return false;
    }
    // stack_frame_arm64.h:
    //   first/last fixed ObjectPtr slots = exit FP - 8 / exit FP - 16
    //   saved caller FP = exit FP + 0
    //   saved caller PC = exit FP + 8
    //   caller SP       = exit FP + 16
    // ExitFrame::VisitObjectPointers() visits the two negative fixed slots even
    // though this synthetic frame has no real pool/code marker. Initialize both
    // to Smi(0), a non-heap tagged value, instead of exposing stale stack bytes
    // to GC. Keep the synthetic frame immediately below entry SP so the next
    // frame observed by StackFrameIterator is the real Dart caller.
    const uintptr_t exit_frame =
        static_cast<uintptr_t>(frame->generated_transition.dart_sp - 2 * sizeof(uint64_t));
    const uint64_t safe_fixed_slot = 0;  // Smi(0).
    const uint64_t caller_fp = frame->generated_transition.caller_fp;
    const uint64_t caller_lr = frame->generated_transition.caller_lr;
    std::memcpy(reinterpret_cast<void*>(exit_frame - 2 * sizeof(uint64_t)), &safe_fixed_slot,
                sizeof(safe_fixed_slot));
    std::memcpy(reinterpret_cast<void*>(exit_frame - sizeof(uint64_t)), &safe_fixed_slot,
                sizeof(safe_fixed_slot));
    std::memcpy(reinterpret_cast<void*>(exit_frame), &caller_fp, sizeof(caller_fp));
    std::memcpy(reinterpret_cast<void*>(exit_frame + sizeof(uint64_t)), &caller_lr,
                sizeof(caller_lr));
    frame->generated_transition.exit_frame = exit_frame;
    return true;
}

bool RootRoleActive(const GeneratedRootBinding& binding, bool include_result) {
    return binding.role == GeneratedRootRole::kInput ||
           (include_result && binding.role == GeneratedRootRole::kResult);
}

bool IsResultLocation(const DispatchFrame& frame, const dartplant::abi::DartAbiLocation& location) {
    if (frame.invocation.call_layout == nullptr) return false;
    const auto& result = frame.invocation.call_layout->result.location;
    for (uint8_t index = 0; index < result.count; ++index) {
        if (result.locations[index] == location) return true;
    }
    return false;
}

bool PinGeneratedRoots(DispatchFrame* frame, bool include_result,
                       bool reuse_cached_inputs = false) {
    if (frame == nullptr || frame->invocation.vm_adapter == nullptr ||
        frame->generated_root_lease != nullptr || frame->generated_root_count == 0) {
        return false;
    }
    for (uint32_t index = 0; index < frame->generated_root_count; ++index) {
        const auto& binding = frame->generated_roots[index];
        uint64_t value = 0;
        if (RootRoleActive(binding, include_result)) {
            if (reuse_cached_inputs && binding.role == GeneratedRootRole::kInput) {
                value = frame->generated_root_values[index];
            } else if (!ReadGeneratedRootLocation(*frame, binding.location, &value)) {
                return false;
            }
        }
        frame->generated_root_values[index] = value;
    }
    const bool pinned =
        dartplant::VmAdapterPinGeneratedRoots(
            frame->invocation.vm_adapter, frame->generated_root_values.data(),
            frame->generated_root_count, &frame->generated_root_lease) == DARTPLANT_OK;
    if (pinned) frame->invocation.generated_root_lease = frame->generated_root_lease;
    return pinned;
}

bool SyncGeneratedRootsToContext(DispatchFrame* frame, bool include_result,
                                 bool preserve_result_locations = false) {
    if (frame == nullptr || frame->invocation.vm_adapter == nullptr ||
        frame->generated_root_lease == nullptr) {
        return false;
    }
    for (uint32_t index = 0; index < frame->generated_root_count; ++index) {
        const auto& binding = frame->generated_roots[index];
        if (!RootRoleActive(binding, include_result)) continue;
        if (preserve_result_locations && binding.role == GeneratedRootRole::kInput &&
            IsResultLocation(*frame, binding.location)) {
            continue;
        }
        uint64_t value = 0;
        if (dartplant::VmAdapterGeneratedRootGet(frame->invocation.vm_adapter,
                                                 frame->generated_root_lease, index,
                                                 &value) != DARTPLANT_OK ||
            !WriteGeneratedRootLocation(frame, binding.location, value)) {
            return false;
        }
        frame->generated_root_values[index] = value;
    }
    return true;
}

bool SyncContextToGeneratedRoots(DispatchFrame* frame, bool include_result) {
    if (frame == nullptr || frame->invocation.vm_adapter == nullptr ||
        frame->generated_root_lease == nullptr) {
        return false;
    }
    (void) include_result;
    // Invocation setters update both the raw context and its persistent root.
    // A moving GC can make the raw context stale while native code is running,
    // so the root lease must remain authoritative at bridge exit.
    return true;
}

bool EnterGeneratedVmBridge(DispatchFrame* frame, bool include_result,
                            bool preserve_result_locations = false) {
    if (frame == nullptr || frame->invocation.vm_adapter == nullptr ||
        frame->generated_root_lease == nullptr) {
        return false;
    }
    if (!PrepareGeneratedExitFrame(frame)) return false;
    if (dartplant::VmAdapterEnterGeneratedToNative(frame->invocation.vm_adapter,
                                                   &frame->generated_transition,
                                                   frame->generated_root_lease) != DARTPLANT_OK) {
        return false;
    }
    if (dartplant_vm_enter_scope(frame->invocation.vm_adapter) != DARTPLANT_OK) {
        if (dartplant::VmAdapterLeaveNativeToGenerated(
                frame->invocation.vm_adapter, &frame->generated_transition,
                frame->generated_root_lease) != DARTPLANT_OK) {
            FatalGeneratedBridgeFailure(
                "generated/native bridge could not recover after scope entry failure");
        }
        return false;
    }
    frame->invocation.vm_scope_entered = true;
    frame->invocation.generated_vm_bridge_active = true;
    if (!SyncGeneratedRootsToContext(frame, include_result, preserve_result_locations)) {
        if (dartplant_vm_leave_scope(frame->invocation.vm_adapter) != DARTPLANT_OK) {
            FatalGeneratedBridgeFailure(
                "generated/native bridge could not leave the VM scope after root sync failure");
        }
        frame->invocation.vm_scope_entered = false;
        frame->invocation.generated_vm_bridge_active = false;
        if (dartplant::VmAdapterLeaveNativeToGenerated(
                frame->invocation.vm_adapter, &frame->generated_transition,
                frame->generated_root_lease) != DARTPLANT_OK) {
            FatalGeneratedBridgeFailure(
                "generated/native bridge could not recover after root sync failure");
        }
        return false;
    }
    return true;
}

bool LeaveGeneratedVmBridge(DispatchFrame* frame, bool include_result, bool repin_inputs) {
    if (frame == nullptr || frame->invocation.vm_adapter == nullptr ||
        frame->generated_root_lease == nullptr || !frame->invocation.vm_scope_entered ||
        !frame->invocation.generated_vm_bridge_active) {
        return false;
    }
    if (!SyncContextToGeneratedRoots(frame, include_result)) {
        FatalGeneratedBridgeFailure("generated/native bridge could not sync callback roots");
    }
    if (dartplant_vm_leave_scope(frame->invocation.vm_adapter) != DARTPLANT_OK) {
        FatalGeneratedBridgeFailure("generated/native bridge could not leave the VM scope");
    }
    frame->invocation.vm_scope_entered = false;
    frame->invocation.generated_vm_bridge_active = false;
    if (dartplant::VmAdapterLeaveNativeToGenerated(frame->invocation.vm_adapter,
                                                   &frame->generated_transition,
                                                   frame->generated_root_lease) != DARTPLANT_OK) {
        FatalGeneratedBridgeFailure("generated/native bridge could not return to generated state");
    }
    if (dartplant::VmAdapterUnpinGeneratedRoots(
            frame->invocation.vm_adapter, frame->generated_root_lease,
            frame->generated_root_values.data(), frame->generated_root_count) != DARTPLANT_OK) {
        FatalGeneratedBridgeFailure("generated/native bridge could not release callback roots");
    }
    frame->generated_root_lease = nullptr;
    frame->invocation.generated_root_lease = nullptr;
    for (uint32_t index = 0; index < frame->generated_root_count; ++index) {
        const auto& binding = frame->generated_roots[index];
        if (!RootRoleActive(binding, include_result)) continue;
        if (!WriteGeneratedRootLocation(frame, binding.location,
                                        frame->generated_root_values[index])) {
            FatalGeneratedBridgeFailure("generated/native bridge could not restore callback roots");
        }
    }
    if (repin_inputs && !PinGeneratedRoots(frame, false)) {
        FatalGeneratedBridgeFailure("generated/native bridge could not repin original arguments");
    }
    return true;
}

bool DropPersistentGeneratedRoots(DispatchFrame* frame, bool include_result,
                                  bool restore_context = true) {
    if (frame == nullptr || frame->generated_root_lease == nullptr ||
        frame->invocation.vm_adapter == nullptr) {
        return true;
    }
    if (frame->invocation.vm_scope_entered || frame->invocation.generated_vm_bridge_active) {
        return false;
    }
    if (dartplant::VmAdapterUnpinGeneratedRoots(
            frame->invocation.vm_adapter, frame->generated_root_lease,
            frame->generated_root_values.data(), frame->generated_root_count) != DARTPLANT_OK) {
        FatalGeneratedBridgeFailure("generated root lease could not be released");
    }
    frame->generated_root_lease = nullptr;
    frame->invocation.generated_root_lease = nullptr;
    if (!restore_context) return true;
    for (uint32_t index = 0; index < frame->generated_root_count; ++index) {
        const auto& binding = frame->generated_roots[index];
        if (!RootRoleActive(binding, include_result)) continue;
        if (!WriteGeneratedRootLocation(frame, binding.location,
                                        frame->generated_root_values[index])) {
            FatalGeneratedBridgeFailure("generated roots could not be restored to Dart context");
        }
    }
    return true;
}

void RefreshIdentityState(DartPlantInvocation* invocation) {
    if (invocation == nullptr) return;
    invocation->identity_ambiguous =
        invocation->code_target != nullptr && invocation->code_target->IsShared();
    invocation->closure_receiver_in_x0 =
        !invocation->identity_ambiguous && invocation->requested_method != nullptr &&
        invocation->requested_method->function != nullptr &&
        invocation->requested_method->function->closure_call_entry_only &&
        invocation->requested_method->record.entry_kind == DARTPLANT_ENTRY_DEFAULT;
    if (invocation->identity_ambiguous) {
        // A verified ABI or hidden closure-receiver contract belongs to one
        // logical Function. Once the physical entry target is shared, semantic
        // interpretation must stop even when installation happened earlier.
        invocation->call_layout = nullptr;
        invocation->closure_receiver_in_x0 = false;
    }
}

void SelectListenerIdentity(DartPlantInvocation* invocation,
                            const std::shared_ptr<dartplant::DartPlantListenerRecord>& listener) {
    if (invocation == nullptr) return;
    invocation->requested_method = listener == nullptr || listener->requested_method == nullptr
                                       ? nullptr
                                       : listener->requested_method.get();
    RefreshIdentityState(invocation);
}

void ClearEnteredListeners(DartPlantInvocation* invocation) {
    if (invocation == nullptr) return;
    for (const auto& listener : invocation->entered_listeners) {
        listener->in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
    invocation->entered_listeners.clear();
}

void ReleaseSnapshot(DartPlantInvocation* invocation) {
    ClearEnteredListeners(invocation);
    dartplant::InvocationExited(invocation->hook);
}

void AbandonFrame(DispatchFrame* frame) {
    if (frame == nullptr) return;
    if (frame->invocation.vm_scope_entered) {
        if (dartplant_vm_leave_scope(frame->invocation.vm_adapter) != DARTPLANT_OK) {
            FatalGeneratedBridgeFailure("exception unwind could not leave the VM scope");
        }
        frame->invocation.vm_scope_entered = false;
    }
    if (frame->invocation.generated_vm_bridge_active && frame->generated_root_lease != nullptr) {
        frame->invocation.generated_vm_bridge_active = false;
        if (dartplant::VmAdapterLeaveNativeToGenerated(
                frame->invocation.vm_adapter, &frame->generated_transition,
                frame->generated_root_lease) != DARTPLANT_OK) {
            FatalGeneratedBridgeFailure("exception unwind could not return to generated state");
        }
    }
    (void) DropPersistentGeneratedRoots(frame, false, false);
    frame->invoke_original_native_frame = 0;
    ReleaseSnapshot(&frame->invocation);
    frame->invocation = {};
    frame->entry_spreg = 0;
    frame->entry_caller_fp = 0;
    frame->generated_root_count = 0;
    frame->generated_root_lease = nullptr;
    frame->generated_transition = {};
}

uint64_t ReadV0Bits(const DartPlantArm64Context& context) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) <= sizeof(context.v[0]));
    std::memcpy(&bits, context.v[0], sizeof(bits));
    return bits;
}

void CaptureReturnChannels(DispatchFrame* frame, uint64_t result0, uint64_t result1,
                           uint64_t fp_result_bits) {
    if (frame == nullptr || frame->invocation.context == nullptr) return;
    frame->invocation.context->x[0] = result0;
    frame->invocation.context->x[1] = result1;
    std::memcpy(frame->invocation.context->v[0], &fp_result_bits, sizeof(fp_result_bits));
}

void FinishFrame(DispatchFrame* frame, uint64_t result0, uint64_t result1,
                 uint64_t fp_result_bits) {
    if (frame == nullptr) return;
    CaptureReturnChannels(frame, result0, result1, fp_result_bits);
    frame->invocation.phase = DARTPLANT_INVOCATION_LEAVE;

    const bool generated_bridge =
        frame->invocation.vm_adapter != nullptr && frame->generated_root_count != 0;
    bool leave_callbacks_enabled = true;
    if (generated_bridge) {
        // Input roots can stay pinned while the original Dart body executes,
        // which keeps the entry-argument snapshot relocatable across moving GC.
        // At the RET boundary the thread is generated again: close that lease,
        // refresh the saved input locations, then create a new lease that also
        // includes the just-produced tagged result before entering native code.
        if (!DropPersistentGeneratedRoots(frame, false, false) ||
            !PinGeneratedRoots(frame, true, true) || !EnterGeneratedVmBridge(frame, true, true)) {
            leave_callbacks_enabled = false;
            (void) DropPersistentGeneratedRoots(frame, true);
        }
    }

    // Pine-style pairing: callbacks that entered are left in reverse order,
    // even if they were concurrently removed after the snapshot was taken.
    if (leave_callbacks_enabled) {
        for (auto it = frame->invocation.entered_listeners.rbegin();
             it != frame->invocation.entered_listeners.rend(); ++it) {
            SelectListenerIdentity(&frame->invocation, *it);
            InvokeCallback((*it)->options.on_leave, &frame->invocation, (*it)->options.user_data);
        }
    }
    if (generated_bridge && frame->invocation.generated_vm_bridge_active) {
        if (!LeaveGeneratedVmBridge(frame, true, false)) {
            FatalGeneratedBridgeFailure(
                "generated/native VM bridge could not safely finish the leave callback");
        }
    } else if (frame->invocation.vm_scope_entered) {
        if (dartplant_vm_leave_scope(frame->invocation.vm_adapter) != DARTPLANT_OK) {
            FatalGeneratedBridgeFailure("callback leave could not leave the VM scope");
        }
        frame->invocation.vm_scope_entered = false;
    }
    if (!DropPersistentGeneratedRoots(frame, true)) {
        FatalGeneratedBridgeFailure("generated/native bridge could not drop its final root lease");
    }
    ReleaseSnapshot(&frame->invocation);
}

}  // namespace

extern "C" DartPlantArm64DispatchResult dartplant_arm64_dispatch_enter(
    DartPlantArm64Context* context, DartPlantHook* hook) {
    DartPlantArm64DispatchResult result{};
    if (context == nullptr || hook == nullptr || !hook->has_method) {
        dartplant::SetLastError("invalid callback dispatch state");
        return result;
    }
    const bool real_dart =
        hook->method_storage != nullptr && hook->method_storage->function != nullptr &&
        hook->method_storage->function->source != dartplant::DartFunctionSource::kSynthetic;
    const bool gated_real_dart = real_dart && hook->published_entry_hook != nullptr;
    if (!hook->active.load(std::memory_order_acquire) && !gated_real_dart) {
        // Reset may have restored the physical entry while a CPU was already
        // fetching the old branch. Published callback stubs retain this hook
        // object, so a stale fetch can safely take the original path.
        result.context = context;
        result.original = hook->backend_installed.load(std::memory_order_acquire)
                              ? hook->backup.load(std::memory_order_acquire)
                          : hook->code_target != nullptr && hook->code_target->entry != 0
                              ? reinterpret_cast<void*>(hook->code_target->entry)
                              : hook->backup.load(std::memory_order_acquire);
        return result;
    }
    DispatchStack* stack = CurrentDispatchStack();
    if (stack == nullptr) {
        // A gated real-Dart CPU has already crossed the physical host patch.
        // If TLS setup fails there is no return/exception bookkeeping capable
        // of releasing that lifetime safely, so deliberately retain the gate
        // entrant instead of allowing a later unhook to reclaim backup.
        if (!gated_real_dart) dartplant::ReleasePublishedHostHookEntrant(hook);
        dartplant::SetLastError("DartPlant per-thread callback storage is unavailable");
        result.context = context;
        result.original = hook->backup.load(std::memory_order_acquire);
        return result;
    }
    if (stack->depth >= kMaxInvocationDepth) {
        if (!gated_real_dart) dartplant::ReleasePublishedHostHookEntrant(hook);
        dartplant::SetLastError("DartPlant invocation depth limit exceeded");
        result.context = context;
        result.original = hook->backup.load(std::memory_order_acquire);
        return result;
    }

    DispatchFrame& frame = stack->frames[stack->depth++];
    frame.context = *context;
    frame.invocation = {};
    frame.entry_spreg = static_cast<uintptr_t>(context->x[15]);
    frame.entry_caller_fp = static_cast<uintptr_t>(context->x[29]);
    frame.invoke_original_native_frame = 0;
    frame.generated_root_count = 0;
    frame.generated_root_lease = nullptr;
    frame.generated_transition = {};
    frame.invocation.hook = hook;
    frame.invocation.requested_method = hook->method_storage.get();
    frame.invocation.code_target = hook->code_target;
    if (frame.invocation.code_target != nullptr) {
        frame.invocation.code_alias_snapshot = frame.invocation.code_target->AliasSnapshot();
    }
    frame.invocation.profile = &hook->profile;
    frame.invocation.call_layout = hook->call_layout.get();
    RefreshIdentityState(&frame.invocation);
    frame.invocation.context = &frame.context;
    frame.invocation.phase = DARTPLANT_INVOCATION_ENTER;
    frame.invocation.depth = stack->depth;
    frame.invocation.validated_null_value = hook->validated_null_value;
    frame.invocation.validated_bool_true_value = hook->validated_bool_true_value;
    frame.invocation.validated_bool_false_value = hook->validated_bool_false_value;

    if (!dartplant::BeginInvocation(hook, &frame.invocation.entered_listeners)) {
        // If a generated entry somehow reached this point while its backend is
        // still installed, retain the entrant forever rather than returning an
        // untracked original invocation that a concurrent unhook could outlive.
        if (!gated_real_dart || !hook->backend_installed.load(std::memory_order_acquire)) {
            dartplant::ReleasePublishedHostHookEntrant(hook);
        }
        result.context = &frame.context;
        result.original = hook->backup.load(std::memory_order_acquire);
        if (real_dart) {
            --stack->depth;
        } else {
            // Native fixtures return through the continuation veneer, which
            // still needs this frame to restore registers and retire the
            // passthrough return.
            frame.invocation.hook = nullptr;
        }
        return result;
    }
    // BeginInvocation has acquired HookRecord::in_flight, but keep the local
    // gate entrant until the exception bridge is known-good. Any setup failure
    // before that point may have to execute original without normal DartPlant
    // return bookkeeping; retaining the entrant then conservatively prevents
    // physical backend teardown for process life.

    // BeginInvocation is the entry-stub quiescence pin. Reset cannot detach
    // this hook or its VM adapter after this point until FinishFrame retires it.
    frame.invocation.vm_adapter = hook->vm_adapter;
    const bool late_shared_fail_closed =
        frame.invocation.identity_ambiguous && !hook->shared_code_opt_in;
    const bool has_callbacks = !frame.invocation.entered_listeners.empty();
    const bool generated_bridge = real_dart && has_callbacks &&
                                  frame.invocation.vm_adapter != nullptr &&
                                  !late_shared_fail_closed;

    if (generated_bridge) {
        if (!BuildGeneratedRootBindings(&frame) || !PinGeneratedRoots(&frame, false) ||
            !EnterGeneratedVmBridge(&frame, false)) {
            (void) DropPersistentGeneratedRoots(&frame, false);
            ReleaseSnapshot(&frame.invocation);
            --stack->depth;
            result.context = &frame.context;
            result.original = hook->backup.load(std::memory_order_acquire);
            return result;
        }
    } else if (has_callbacks && !late_shared_fail_closed &&
               frame.invocation.vm_adapter != nullptr) {
        if (dartplant_vm_enter_scope(frame.invocation.vm_adapter) != DARTPLANT_OK) {
            ReleaseSnapshot(&frame.invocation);
            --stack->depth;
            result.context = &frame.context;
            result.original = hook->backup.load(std::memory_order_acquire);
            return result;
        }
        frame.invocation.vm_scope_entered = true;
    }

    if (real_dart && !dartplant::EnsureArm64ExceptionBridge(hook, frame.context)) {
        // The physical entry/RET patches remain safe passthrough instrumentation,
        // but callbacks must not start unless Dart non-local unwinds can retire
        // their pending invocation state. A generated VM bridge is already in
        // native state here, so release it before resuming Dart.
        if (generated_bridge && frame.invocation.generated_vm_bridge_active) {
            (void) LeaveGeneratedVmBridge(&frame, false, false);
        } else if (frame.invocation.vm_scope_entered) {
            if (dartplant_vm_leave_scope(frame.invocation.vm_adapter) != DARTPLANT_OK) {
                FatalGeneratedBridgeFailure("callback setup could not leave the VM scope");
            }
            frame.invocation.vm_scope_entered = false;
        }
        (void) DropPersistentGeneratedRoots(&frame, false);
        ReleaseSnapshot(&frame.invocation);
        --stack->depth;
        result.context = &frame.context;
        result.original = hook->backup.load(std::memory_order_acquire);
        return result;
    }

    if (gated_real_dart) {
        // JumpToFrame is now publication-safe and HookRecord::in_flight covers
        // the complete Dart body, so the short gate-to-dispatch handoff pin is
        // no longer needed.
        dartplant::ReleasePublishedHostHookEntrant(hook);
    }

    if (late_shared_fail_closed) {
        // The hook was installed while this target appeared unique, but a later
        // alias made it shared. Without explicit shared-code opt-in, execute no
        // user callback at all; keep only the hook invocation pin until the
        // original returns through FinishFrame().
        ClearEnteredListeners(&frame.invocation);
        dartplant::SetLastError(
            "callback target became shared after installation; callbacks were bypassed");
        result.context = &frame.context;
        result.original = hook->backup.load(std::memory_order_acquire);
        return result;
    }

    size_t entered_count = 0;
    for (const auto& listener : frame.invocation.entered_listeners) {
        SelectListenerIdentity(&frame.invocation, listener);
        if (frame.invocation.identity_ambiguous && !hook->shared_code_opt_in) break;
        ++entered_count;
        InvokeCallback(listener->options.on_enter, &frame.invocation, listener->options.user_data);
        if (frame.invocation.skip_original) break;
    }
    for (size_t index = entered_count; index < frame.invocation.entered_listeners.size(); ++index) {
        frame.invocation.entered_listeners[index]->in_flight.fetch_sub(1,
                                                                       std::memory_order_acq_rel);
    }
    frame.invocation.entered_listeners.resize(entered_count);

    if (generated_bridge && frame.invocation.generated_vm_bridge_active) {
        const bool skip_original = frame.invocation.skip_original;
        if (!LeaveGeneratedVmBridge(&frame, skip_original, !skip_original)) {
            dartplant::SetLastError(
                "generated/native VM bridge could not safely finish the enter callback");
            (void) DropPersistentGeneratedRoots(&frame, skip_original);
            ClearEnteredListeners(&frame.invocation);
            dartplant::InvocationExited(hook);
            --stack->depth;
            result.context = &frame.context;
            result.original =
                skip_original ? nullptr : hook->backup.load(std::memory_order_acquire);
            return result;
        }
    }

    if (frame.invocation.skip_original) {
        FinishFrame(&frame, frame.context.x[0], frame.context.x[1], ReadV0Bits(frame.context));
        --stack->depth;
        result.context = &frame.context;
        return result;
    }

    result.context = &frame.context;
    result.original = hook->backup.load(std::memory_order_acquire);
    return result;
}

extern "C" uint8_t dartplant_arm64_prepare_invoke_original_frame(uintptr_t native_frame_sp) {
    DispatchFrame* frame = CurrentFrame();
    if (frame == nullptr || frame->invocation.context == nullptr || native_frame_sp == 0 ||
        frame->invoke_original_native_frame != 0) {
        dartplant::SetLastError("invoke-original native frame has no active Dart invocation");
        return 0;
    }
    frame->invoke_original_native_frame = native_frame_sp;
    return 1;
}

extern "C" DartPlantArm64ReturnDispatchResult dartplant_arm64_dispatch_return_from_payload(
    dartplant::DartCodePayload* payload, uint64_t result0, uint64_t result1,
    uint64_t fp_result_bits, uintptr_t return_lr, uintptr_t return_spreg, uintptr_t return_fp) {
    DartPlantArm64ReturnDispatchResult output{};
    DispatchFrame* frame = CurrentFrame();
    if (frame == nullptr || frame->invocation.context == nullptr ||
        frame->invocation.code_target == nullptr ||
        frame->invocation.code_target->payload.get() != payload ||
        frame->invocation.context->x[30] != return_lr || frame->entry_spreg != return_spreg ||
        frame->entry_caller_fp != return_fp) {
        return output;
    }

    CaptureReturnChannels(frame, result0, result1, fp_result_bits);
    output.context = frame->invocation.context;
    if (frame->invoke_original_native_frame != 0) {
        output.resume_native_sp = frame->invoke_original_native_frame;
        frame->invoke_original_native_frame = 0;
        return output;
    }

    FinishFrame(frame, result0, result1, fp_result_bits);
    output.context = frame->invocation.context;
    if (DispatchStack* stack = CurrentDispatchStack(); stack != nullptr && stack->depth != 0) {
        --stack->depth;
    }
    return output;
}

extern "C" DartPlantArm64ReturnDispatchResult dartplant_arm64_dispatch_return_from_hook(
    DartPlantHook* hook, uint64_t result0, uint64_t result1, uint64_t fp_result_bits,
    uintptr_t return_lr, uintptr_t return_spreg, uintptr_t return_fp) {
    auto* payload = hook == nullptr || hook->code_target == nullptr
                        ? nullptr
                        : hook->code_target->payload.get();
    return dartplant_arm64_dispatch_return_from_payload(payload, result0, result1, fp_result_bits,
                                                        return_lr, return_spreg, return_fp);
}

extern "C" void dartplant_arm64_dispatch_exception_unwind(uintptr_t target_spreg,
                                                          uintptr_t target_fp) {
    if (target_spreg == 0 && target_fp == 0) return;
    DispatchStack* stack = CurrentDispatchStack();
    if (stack == nullptr) return;
    while (stack->depth != 0) {
        DispatchFrame& frame = stack->frames[stack->depth - 1];
        // Dart stacks grow downward. A handler in the hooked Function keeps a
        // frame below its caller FP/SPREG, while unwinding out of that Function
        // reaches its caller frame (or above). Prefer FP because it is stable
        // across outgoing-argument layout changes; keep SPREG as the fallback
        // for frameless/top-level cases.
        const bool unwound_by_fp =
            frame.entry_caller_fp != 0 && target_fp != 0 && target_fp >= frame.entry_caller_fp;
        const bool unwound_by_sp =
            frame.entry_spreg != 0 && target_spreg != 0 && target_spreg > frame.entry_spreg;
        if (!unwound_by_fp && !unwound_by_sp) break;
        AbandonFrame(&frame);
        --stack->depth;
    }
}

extern "C" DartPlantArm64LeaveResult dartplant_arm64_dispatch_leave_from_tls(
    uint64_t result0, uint64_t result1, uint64_t fp_result_bits) {
    DartPlantArm64LeaveResult output{};
    DispatchFrame* frame = CurrentFrame();
    if (frame == nullptr || frame->invocation.context == nullptr) {
        dartplant::SetLastError("leave callback has no active invocation");
        output.result = result0;
        return output;
    }
    FinishFrame(frame, result0, result1, fp_result_bits);
    output.context = frame->invocation.context;
    output.result = output.context->x[0];
    if (DispatchStack* stack = CurrentDispatchStack(); stack != nullptr && stack->depth != 0) {
        --stack->depth;
    }
    return output;
}
