#include "simple_facade_consumer.h"

#include <android/log.h>

#include <atomic>
#include <bit>

#include "dartplant/adapters/dobby.h"
#include "dartplant/dartplant.h"
#include "dartplant/hook.h"
#include "dartplant/host_api.h"
#include "dartplant/invocation.h"

namespace {

constexpr char kTag[] = "DartPlantSimpleConsumer";

DartPlantHookHandle* g_primary = nullptr;
DartPlantHookHandle* g_observer = nullptr;
std::atomic_uint64_t g_enter{0};
std::atomic_uint64_t g_leave{0};
std::atomic_uint64_t g_observer_enter{0};
std::atomic_uint64_t g_failures{0};

void Fail(const char* operation) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s: %s", operation, dartplant_last_error());
}

void PrimaryEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue left{};
    DartPlantValue right{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 2 ||
        dartplant_invocation_get_argument(invocation, 0, &left) != DARTPLANT_OK ||
        dartplant_invocation_get_argument(invocation, 1, &right) != DARTPLANT_OK ||
        left.kind != DARTPLANT_VALUE_DOUBLE || right.kind != DARTPLANT_VALUE_DOUBLE) {
        Fail("typed enter decode");
        return;
    }
    double value = std::bit_cast<double>(left.raw) + 2.0;
    left.raw = std::bit_cast<uint64_t>(value);
    if (dartplant_invocation_set_argument(invocation, 0, &left) != DARTPLANT_OK) {
        Fail("typed enter rewrite");
        return;
    }
    g_enter.fetch_add(1, std::memory_order_relaxed);
}

void PrimaryLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_DOUBLE) {
        Fail("typed leave decode");
        return;
    }
    double value = std::bit_cast<double>(result.raw) + 20.0;
    result.raw = std::bit_cast<uint64_t>(value);
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        Fail("typed leave rewrite");
        return;
    }
    g_leave.fetch_add(1, std::memory_order_relaxed);
}

void ObserverEnter(DartPlantInvocation* invocation, void*) {
    if (dartplant_invocation_has_verified_abi(invocation) == 0) {
        Fail("typed observer");
        return;
    }
    g_observer_enter.fetch_add(1, std::memory_order_relaxed);
}

bool RemoveHandle(DartPlantHookHandle** handle) {
    if (handle == nullptr || *handle == nullptr) return true;
    if (dartplant_unhook_handle(*handle) != DARTPLANT_OK ||
        dartplant_hook_handle_is_idle(*handle) == 0) {
        return false;
    }
    dartplant_release_hook_handle(*handle);
    *handle = nullptr;
    return true;
}

}  // namespace

int32_t dartplant_simple_consumer_install() {
    if (g_primary != nullptr || g_observer != nullptr) return DARTPLANT_ALREADY_HOOKED;

    g_enter.store(0, std::memory_order_relaxed);
    g_leave.store(0, std::memory_order_relaxed);
    g_observer_enter.store(0, std::memory_order_relaxed);
    g_failures.store(0, std::memory_order_relaxed);

    const DartPlantHostApi* host_api = dartplant_dobby_host_api();
    if (host_api == nullptr) return DARTPLANT_HOST_API_UNAVAILABLE;
    const DartPlantInitInfo init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = host_api,
        .artifact_bundle = nullptr,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    DartPlantStatus status = dartplant_init(&init);
    if (status != DARTPLANT_OK) return status;

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = "verifiedAbiDouble",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    status = dartplant_find_method(&query, &method);
    if (status != DARTPLANT_OK) {
        dartplant_shutdown();
        return status;
    }

    const DartPlantHookOptions primary_options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = PrimaryEnter,
        .on_leave = PrimaryLeave,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    status = dartplant_hook_method(method, &primary_options, &g_primary);
    if (status == DARTPLANT_OK) {
        const DartPlantHookOptions observer_options = {
            .struct_size = sizeof(DartPlantHookOptions),
            .flags = 0,
            .on_enter = ObserverEnter,
            .on_leave = nullptr,
            .user_data = nullptr,
            .vm_adapter = nullptr,
        };
        status = dartplant_hook_method(method, &observer_options, &g_observer);
    }
    dartplant_release_method(method);

    const bool ready = status == DARTPLANT_OK && g_primary != nullptr && g_observer != nullptr &&
                       dartplant_hook_handle_is_active(g_primary) != 0 &&
                       dartplant_hook_handle_is_active(g_observer) != 0;
    __android_log_print(ready ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "simple facade typed install ready=%u status=%d error=%s",
                        static_cast<unsigned>(ready), status,
                        ready ? "none" : dartplant_last_error());
    if (!ready) {
        dartplant_simple_consumer_cleanup();
        return status == DARTPLANT_OK ? DARTPLANT_HOOK_FAILED : status;
    }
    return DARTPLANT_OK;
}

uint64_t dartplant_simple_consumer_stage1() {
    const uint64_t enter = g_enter.load(std::memory_order_relaxed);
    const uint64_t leave = g_leave.load(std::memory_order_relaxed);
    const uint64_t observer = g_observer_enter.load(std::memory_order_relaxed);
    const uint64_t failures = g_failures.load(std::memory_order_relaxed);
    const bool primary_active = g_primary != nullptr && dartplant_hook_handle_is_active(g_primary);
    const bool observer_active =
        g_observer != nullptr && dartplant_hook_handle_is_active(g_observer);
    const bool observer_removed = observer_active && RemoveHandle(&g_observer);
    const bool passed = enter == 1 && leave == 1 && observer == 1 && failures == 0 &&
                        primary_active && observer_removed && g_observer == nullptr;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
        "simple facade typed stage1 enter=%llu leave=%llu observer=%llu failures=%llu primary_active=%u observer_removed=%u passed=%u",
        static_cast<unsigned long long>(enter), static_cast<unsigned long long>(leave),
        static_cast<unsigned long long>(observer), static_cast<unsigned long long>(failures),
        static_cast<unsigned>(primary_active), static_cast<unsigned>(observer_removed),
        static_cast<unsigned>(passed));
    return passed ? 1 : 0;
}

uint64_t dartplant_simple_consumer_stage2() {
    const uint64_t enter = g_enter.load(std::memory_order_relaxed);
    const uint64_t leave = g_leave.load(std::memory_order_relaxed);
    const uint64_t observer = g_observer_enter.load(std::memory_order_relaxed);
    const uint64_t failures = g_failures.load(std::memory_order_relaxed);
    const bool primary_active = g_primary != nullptr && dartplant_hook_handle_is_active(g_primary);
    const bool primary_removed = primary_active && RemoveHandle(&g_primary);
    if (primary_removed) dartplant_shutdown();
    const bool shutdown = dartplant_is_initialized() == 0;
    const bool passed =
        enter == 2 && leave == 2 && observer == 1 && failures == 0 && primary_removed && shutdown;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
        "simple facade typed stage2 enter=%llu leave=%llu observer=%llu failures=%llu primary_removed=%u shutdown=%u passed=%u",
        static_cast<unsigned long long>(enter), static_cast<unsigned long long>(leave),
        static_cast<unsigned long long>(observer), static_cast<unsigned long long>(failures),
        static_cast<unsigned>(primary_removed), static_cast<unsigned>(shutdown),
        static_cast<unsigned>(passed));
    return passed ? 1 : 0;
}

void dartplant_simple_consumer_cleanup() {
    (void) RemoveHandle(&g_observer);
    (void) RemoveHandle(&g_primary);
    if (dartplant_is_initialized() != 0) dartplant_shutdown();
}
