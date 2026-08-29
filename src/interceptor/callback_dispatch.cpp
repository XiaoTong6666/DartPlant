// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstring>

#include "runtime/runtime_internal.h"

namespace {

constexpr uint32_t kMaxInvocationDepth = 64;

struct DispatchFrame {
    DartPlantArm64Context context{};
    DartPlantInvocation invocation{};
    uintptr_t entry_spreg = 0;
    uintptr_t entry_caller_fp = 0;
    uintptr_t invoke_original_native_frame = 0;
};

thread_local std::array<DispatchFrame, kMaxInvocationDepth> g_frames;
thread_local uint32_t g_depth = 0;

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

DispatchFrame* CurrentFrame() { return g_depth == 0 ? nullptr : &g_frames[g_depth - 1]; }

void RefreshIdentityState(DartPlantInvocation* invocation) {
    if (invocation == nullptr) return;
    invocation->identity_ambiguous =
        invocation->code_target != nullptr && invocation->code_target->IsShared();
    if (invocation->identity_ambiguous) {
        // A verified ABI belongs to one logical Function. Once the physical
        // CodeTarget is known to be shared, typed interpretation must stop even
        // when the hook was installed while the target still looked unique.
        invocation->call_layout = nullptr;
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
        dartplant_vm_leave_scope(frame->invocation.vm_adapter);
        frame->invocation.vm_scope_entered = false;
    }
    frame->invoke_original_native_frame = 0;
    ReleaseSnapshot(&frame->invocation);
    frame->invocation = {};
    frame->entry_spreg = 0;
    frame->entry_caller_fp = 0;
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

    // Pine-style pairing: callbacks that entered are left in reverse order,
    // even if they were concurrently removed after the snapshot was taken.
    for (auto it = frame->invocation.entered_listeners.rbegin();
         it != frame->invocation.entered_listeners.rend(); ++it) {
        SelectListenerIdentity(&frame->invocation, *it);
        InvokeCallback((*it)->options.on_leave, &frame->invocation, (*it)->options.user_data);
    }
    if (frame->invocation.vm_scope_entered) {
        dartplant_vm_leave_scope(frame->invocation.vm_adapter);
        frame->invocation.vm_scope_entered = false;
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
    if (real_dart && !dartplant::EnsureArm64ExceptionBridge(hook, *context)) {
        // The physical entry/RET patches remain safe passthrough instrumentation,
        // but callbacks must not start unless Dart non-local unwinds can retire
        // their pending invocation state.
        result.context = context;
        result.original = hook->backup;
        return result;
    }
    if (g_depth >= kMaxInvocationDepth) {
        dartplant::SetLastError("DartPlant invocation depth limit exceeded");
        result.context = context;
        result.original = hook->backup;
        return result;
    }

    DispatchFrame& frame = g_frames[g_depth++];
    frame.context = *context;
    frame.invocation = {};
    frame.entry_spreg = static_cast<uintptr_t>(context->x[15]);
    frame.entry_caller_fp = static_cast<uintptr_t>(context->x[29]);
    frame.invoke_original_native_frame = 0;
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
    frame.invocation.depth = g_depth;
    frame.invocation.vm_adapter = hook->vm_adapter;
    frame.invocation.validated_null_value = hook->validated_null_value;
    frame.invocation.validated_bool_true_value = hook->validated_bool_true_value;
    frame.invocation.validated_bool_false_value = hook->validated_bool_false_value;

    const bool late_shared_fail_closed =
        frame.invocation.identity_ambiguous && !hook->shared_code_opt_in;

    if (!late_shared_fail_closed && frame.invocation.vm_adapter != nullptr) {
        if (dartplant_vm_enter_scope(frame.invocation.vm_adapter) != DARTPLANT_OK) {
            --g_depth;
            result.context = &frame.context;
            result.original = hook->backup;
            return result;
        }
        frame.invocation.vm_scope_entered = true;
    }

    if (!dartplant::BeginInvocation(hook, &frame.invocation.entered_listeners)) {
        if (frame.invocation.vm_scope_entered) {
            dartplant_vm_leave_scope(frame.invocation.vm_adapter);
            frame.invocation.vm_scope_entered = false;
        }
        result.context = &frame.context;
        result.original = hook->backup;
        if (real_dart) {
            // Real Dart returns are intercepted at RET sites. If no invocation
            // pin was acquired (for example during unhook), do not leave a TLS
            // frame behind for the passthrough return veneer.
            --g_depth;
        } else {
            // Synthetic native fixtures still use their LR continuation.
            frame.invocation.hook = nullptr;
        }
        return result;
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
        result.original = hook->backup;
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

    if (frame.invocation.skip_original) {
        FinishFrame(&frame, frame.context.x[0], frame.context.x[1], ReadV0Bits(frame.context));
        --g_depth;
        result.context = &frame.context;
        return result;
    }

    result.context = &frame.context;
    result.original = hook->backup;
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

extern "C" DartPlantArm64ReturnDispatchResult dartplant_arm64_dispatch_return_from_hook(
    DartPlantHook* hook, uint64_t result0, uint64_t result1, uint64_t fp_result_bits) {
    DartPlantArm64ReturnDispatchResult output{};
    DispatchFrame* frame = CurrentFrame();
    if (frame == nullptr || frame->invocation.context == nullptr ||
        frame->invocation.hook != hook) {
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
    --g_depth;
    return output;
}

extern "C" void dartplant_arm64_dispatch_exception_unwind(uintptr_t target_spreg,
                                                          uintptr_t target_fp) {
    if (target_spreg == 0 && target_fp == 0) return;
    while (g_depth != 0) {
        DispatchFrame& frame = g_frames[g_depth - 1];
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
        --g_depth;
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
    --g_depth;
    return output;
}
