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

void SelectListenerIdentity(DartPlantInvocation* invocation,
                            const std::shared_ptr<dartplant::DartPlantListenerRecord>& listener) {
    if (invocation == nullptr) return;
    invocation->requested_method = listener == nullptr || listener->requested_method == nullptr
                                       ? nullptr
                                       : listener->requested_method.get();
    invocation->identity_ambiguous =
        invocation->code_target != nullptr && invocation->code_target->IsShared();
}

void ReleaseSnapshot(DartPlantInvocation* invocation) {
    for (const auto& listener : invocation->entered_listeners) {
        listener->in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
    invocation->entered_listeners.clear();
    dartplant::InvocationExited(invocation->hook);
}

uint64_t ReadV0Bits(const DartPlantArm64Context& context) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) <= sizeof(context.v[0]));
    std::memcpy(&bits, context.v[0], sizeof(bits));
    return bits;
}

void FinishFrame(DispatchFrame* frame, uint64_t result0, uint64_t result1,
                 uint64_t fp_result_bits) {
    if (frame == nullptr) return;
    frame->invocation.context->x[0] = result0;
    frame->invocation.context->x[1] = result1;
    std::memcpy(frame->invocation.context->v[0], &fp_result_bits, sizeof(fp_result_bits));
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
    if (g_depth >= kMaxInvocationDepth) {
        dartplant::SetLastError("DartPlant invocation depth limit exceeded");
        result.context = context;
        result.original = hook->backup;
        return result;
    }

    DispatchFrame& frame = g_frames[g_depth++];
    frame.context = *context;
    frame.invocation = {};
    frame.invocation.hook = hook;
    frame.invocation.requested_method = hook->method_storage.get();
    frame.invocation.code_target = hook->code_target;
    if (frame.invocation.code_target != nullptr) {
        frame.invocation.code_alias_snapshot = frame.invocation.code_target->AliasSnapshot();
    }
    frame.invocation.identity_ambiguous =
        frame.invocation.code_target != nullptr && frame.invocation.code_target->IsShared();
    frame.invocation.profile = &hook->profile;
    frame.invocation.call_layout = hook->call_layout.get();
    frame.invocation.context = &frame.context;
    frame.invocation.phase = DARTPLANT_INVOCATION_ENTER;
    frame.invocation.depth = g_depth;
    frame.invocation.vm_adapter = hook->vm_adapter;
    frame.invocation.validated_null_value = hook->validated_null_value;
    frame.invocation.validated_bool_true_value = hook->validated_bool_true_value;
    frame.invocation.validated_bool_false_value = hook->validated_bool_false_value;

    if (frame.invocation.vm_adapter != nullptr) {
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
        --g_depth;
        result.context = &frame.context;
        result.original = hook->backup;
        return result;
    }

    size_t entered_count = 0;
    for (const auto& listener : frame.invocation.entered_listeners) {
        ++entered_count;
        SelectListenerIdentity(&frame.invocation, listener);
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
