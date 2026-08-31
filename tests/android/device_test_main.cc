// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>
#include <dobby.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/internal.h"
#include "dartplant/invocation.h"
#include "dartplant/runtime.h"
#include "dartplant/runtime_profile.h"
#include "runtime/runtime_internal.h"

namespace {

using Add = int (*)(int, int);

Add g_original_add = nullptr;
int g_enter_calls = 0;
int g_leave_calls = 0;
int g_listener_calls = 0;
bool g_null_semantic_callback_ok = false;
bool g_raw_context_callback_ok = false;
bool g_dart_pad_branch_ok = false;
std::atomic_uint64_t g_local_gate_replacement_hits{0};
std::atomic<dartplant::PublishedHostHook*> g_generated_gate_hook{nullptr};
std::atomic<DartPlantHook*> g_generation_gate_hook{nullptr};
std::atomic_bool g_generated_gate_hold{false};
std::atomic_bool g_generated_gate_entered{false};

void OnEnter(DartPlantInvocation* invocation, void*) {
    ++g_enter_calls;
    DartPlantValue value{};
    if (dartplant_invocation_get_argument(invocation, 0, &value) == DARTPLANT_OK) {
        value.raw += 10;
        dartplant_invocation_set_argument(invocation, 0, &value);
    }
}

void OnLeave(DartPlantInvocation* invocation, void*) {
    ++g_leave_calls;
    DartPlantValue value{};
    if (dartplant_invocation_get_result(invocation, &value) == DARTPLANT_OK) {
        value.raw += 100;
        dartplant_invocation_set_result(invocation, &value);
    }
}

void SkipWithResult(DartPlantInvocation* invocation, void*) {
    const DartPlantValue value = {DARTPLANT_VALUE_RAW_WORD, 0, 77};
    dartplant_invocation_set_result(invocation, &value);
}

void SkipWithValidatedNull(DartPlantInvocation* invocation, void*) {
    const DartPlantValue null_value = {DARTPLANT_VALUE_NULL, 0, 0};
    DartPlantValue argument{};
    const DartPlantStatus set_argument =
        dartplant_invocation_set_argument(invocation, 0, &null_value);
    const DartPlantStatus get_argument =
        dartplant_invocation_get_argument(invocation, 0, &argument);
    const DartPlantStatus set_result = dartplant_invocation_set_result(invocation, &null_value);
    g_null_semantic_callback_ok = set_argument == DARTPLANT_OK && get_argument == DARTPLANT_OK &&
                                  argument.kind == DARTPLANT_VALUE_NULL &&
                                  set_result == DARTPLANT_OK;
}

void ObserveEnter(DartPlantInvocation*, void*) { ++g_listener_calls; }

void VerifyDartPadBranch(DartPlantInvocation* invocation, void*) {
    uint64_t spreg = 0;
    uintptr_t csp = 0;
#if defined(__aarch64__)
    asm volatile("mov %0, sp" : "=r"(csp));
#endif
    uint64_t argument = 0;
    g_dart_pad_branch_ok =
        dartplant_invocation_get_gp_register(invocation, 15, &spreg) == DARTPLANT_OK &&
        dartplant_invocation_get_gp_register(invocation, 0, &argument) == DARTPLANT_OK &&
        (spreg & 0xfU) == 8 && (csp & 0xfU) == 0 && argument == 5;
    if (dartplant_invocation_set_gp_register(invocation, 0, 77) != DARTPLANT_OK ||
        dartplant_invocation_skip_original(invocation) != DARTPLANT_OK) {
        g_dart_pad_branch_ok = false;
    }
}

extern "C" uint64_t DartPlantDeviceInvokeDartCallbackWithOddSp(DartPlantHook* hook,
                                                               uintptr_t dart_spreg,
                                                               uint64_t argument);
extern "C" int DartPlantDeviceInvokeGeneratedGate(void* target, uintptr_t dart_spreg, int left,
                                                  int right);
extern "C" int DartPlantFixtureAdd(int left, int right);

int Fail(const char* message);

int ExerciseDartPadBranch() {
    constexpr size_t kFakeStackSize = 1U << 20;
    void* mapping =
        mmap(nullptr, kFakeStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return Fail("map deterministic Dart pad stack");

    const uintptr_t end = reinterpret_cast<uintptr_t>(mapping) + kFakeStackSize;
    const uintptr_t dart_spreg = ((end - (64U << 10)) & ~uintptr_t{0xf}) + 8;

    auto target = std::make_shared<dartplant::DartEntryTarget>();
    target->id = reinterpret_cast<uintptr_t>(DartPlantFixtureAdd);
    target->entry = reinterpret_cast<uintptr_t>(DartPlantFixtureAdd);
    target->code_size = 4;
    auto function = std::make_shared<dartplant::DartFunctionHandle>();
    function->identity = {
        .library_uri = "package:fixture/pad.dart",
        .class_name = "Fixture",
        .function_name = "padProbe",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    function->source = dartplant::DartFunctionSource::kSynthetic;
    function->code_target = target;
    target->AddAlias(function->identity);

    DartPlantMethod method{};
    method.record.function_name = "padProbe";
    method.function = function;
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = target;
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.backup = reinterpret_cast<void*>(DartPlantFixtureAdd);
    dartplant_runtime_profile_init_arm64_aot(&hook.profile);

    const DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = VerifyDartPadBranch,
        .on_leave = nullptr,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* listener = nullptr;
    const DartPlantStatus add_status =
        dartplant::AddCallbackListener(&hook, &method, options, 0, &listener);
    g_dart_pad_branch_ok = false;
    const uint64_t result = add_status == DARTPLANT_OK
                                ? DartPlantDeviceInvokeDartCallbackWithOddSp(&hook, dart_spreg, 5)
                                : 0;
    const bool idle = listener != nullptr && dartplant_listener_is_idle(listener) != 0;
    if (listener != nullptr) {
        (void) dartplant_remove_listener(listener);
        dartplant_release_listener(listener);
    }
    munmap(mapping, kFakeStackSize);
    if (add_status != DARTPLANT_OK || result != 77 || !g_dart_pad_branch_ok || !idle) {
        return Fail("deterministic Dart x15 alignment pad");
    }
    return 0;
}

int ExercisePayloadReturnOwnership() {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return Fail("payload return page size");
    void* mapping = mmap(nullptr, static_cast<size_t>(page_size),
                         PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return Fail("map payload return fixture");
    auto* code = static_cast<uint32_t*>(mapping);
    code[0] = 0x540000c0U;  // entry A: b.eq shared RET at +24, else fall through.
    code[1] = 0x14000002U;  // A fallthrough -> A-only RET at +12.
    code[2] = 0xd503201fU;  // unreachable padding.
    code[3] = 0xd65f03c0U;  // A-only RET.
    code[4] = 0x54000040U;  // entry B: b.eq shared RET at +24, else fall through.
    code[5] = 0x14000002U;  // B fallthrough -> B-only RET at +28.
    code[6] = 0xd65f03c0U;  // shared RET.
    code[7] = 0xd65f03c0U;  // B-only RET.

    auto payload = std::make_shared<dartplant::DartCodePayload>();
    payload->id = reinterpret_cast<uintptr_t>(mapping);
    payload->start = payload->id;
    payload->instructions_length = 32;
    payload->pristine_bytes.resize(32);
    std::memcpy(payload->pristine_bytes.data(), code, 32);

    auto first = std::make_shared<dartplant::DartEntryTarget>();
    first->id = payload->start;
    first->entry = first->id;
    first->code_size = 32;
    first->payload = payload;
    auto second = std::make_shared<dartplant::DartEntryTarget>();
    second->id = payload->start + 16;
    second->entry = second->id;
    second->code_size = 16;
    second->payload = payload;

    DartPlantHook first_hook{};
    first_hook.code_target = first;
    DartPlantHook second_hook{};
    second_hook.code_target = second;
    if (dartplant::InstallArm64ReturnInterception(&first_hook) != DARTPLANT_OK ||
        payload->return_patches.size() != 2 || payload->return_interception_consumers != 1 ||
        code[3] == 0xd65f03c0U || code[6] == 0xd65f03c0U || code[7] != 0xd65f03c0U) {
        munmap(mapping, static_cast<size_t>(page_size));
        return Fail("first payload return interception");
    }
    const uint32_t first_only_branch = code[3];
    const uint32_t shared_branch = code[6];
    if (mprotect(mapping, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE | PROT_EXEC) !=
        0) {
        (void) dartplant::RestoreArm64ReturnInterception(&first_hook);
        munmap(mapping, static_cast<size_t>(page_size));
        return Fail("failed sibling return test write permission");
    }
    code[7] = 0xd503201fU;
    __builtin___clear_cache(reinterpret_cast<char*>(&code[7]), reinterpret_cast<char*>(&code[8]));
    if (dartplant::InstallArm64ReturnInterception(&second_hook) != DARTPLANT_HOOK_FAILED ||
        payload->return_patches.size() != 2 || payload->return_interception_consumers != 1 ||
        code[3] != first_only_branch || code[6] != shared_branch || code[7] != 0xd503201fU) {
        code[7] = 0xd65f03c0U;
        (void) dartplant::RestoreArm64ReturnInterception(&first_hook);
        munmap(mapping, static_cast<size_t>(page_size));
        return Fail("failed sibling return interception rollback");
    }
    code[7] = 0xd65f03c0U;
    __builtin___clear_cache(reinterpret_cast<char*>(&code[7]), reinterpret_cast<char*>(&code[8]));
    if (dartplant::InstallArm64ReturnInterception(&second_hook) != DARTPLANT_OK ||
        payload->return_patches.size() != 3 || payload->return_interception_consumers != 2 ||
        code[3] != first_only_branch || code[6] != shared_branch || code[7] == 0xd65f03c0U) {
        (void) dartplant::RestoreArm64ReturnInterception(&first_hook);
        munmap(mapping, static_cast<size_t>(page_size));
        return Fail("sibling payload return interception");
    }
    const uint32_t foreign_instruction = 0xd503201fU;
    // Return interception deliberately restores the containing code page to
    // RX after every managed write. A foreign writer would have to acquire
    // write permission independently too; mirror that here instead of
    // faulting the test itself by assigning through an RX mapping.
    if (mprotect(mapping, static_cast<size_t>(page_size), PROT_READ | PROT_WRITE | PROT_EXEC) !=
        0) {
        (void) dartplant::RestoreArm64ReturnInterception(&first_hook);
        (void) dartplant::RestoreArm64ReturnInterception(&second_hook);
        munmap(mapping, static_cast<size_t>(page_size));
        return Fail("foreign shared RET test write permission");
    }
    code[6] = foreign_instruction;
    __builtin___clear_cache(reinterpret_cast<char*>(&code[6]), reinterpret_cast<char*>(&code[7]));
    if (dartplant::RestoreArm64ReturnInterception(&first_hook) ||
        payload->return_interception_consumers != 2 || payload->return_patches.size() != 3 ||
        code[6] != foreign_instruction) {
        code[6] = shared_branch;
        (void) dartplant::RestoreArm64ReturnInterception(&first_hook);
        (void) dartplant::RestoreArm64ReturnInterception(&second_hook);
        munmap(mapping, static_cast<size_t>(page_size));
        return Fail("foreign shared RET patch ownership rejection");
    }
    code[6] = shared_branch;
    void* const published_return_entry = payload->return_entry;
    if (!dartplant::RestoreArm64ReturnInterception(&first_hook) ||
        payload->return_interception_consumers != 1 || payload->return_patches.size() != 2 ||
        code[3] != 0xd65f03c0U || code[6] != shared_branch || code[7] == 0xd65f03c0U ||
        !dartplant::RestoreArm64ReturnInterception(&second_hook) ||
        payload->return_interception_consumers != 0 || !payload->return_patches.empty() ||
        code[6] != 0xd65f03c0U || code[7] != 0xd65f03c0U ||
        payload->return_entry != published_return_entry || payload->return_entry == nullptr ||
        !payload->return_entry_published) {
        munmap(mapping, static_cast<size_t>(page_size));
        return Fail("payload return interception release");
    }
    munmap(mapping, static_cast<size_t>(page_size));
    return 0;
}

void ObserveRawContext(DartPlantInvocation* invocation, void*) {
    uint64_t x0 = 0;
    DartPlantValue typed{};
    g_raw_context_callback_ok =
        dartplant_invocation_has_verified_abi(invocation) == 0 &&
        dartplant_invocation_get_gp_register(invocation, 0, &x0) == DARTPLANT_OK && x0 == 2 &&
        dartplant_invocation_get_argument(invocation, 0, &typed) == DARTPLANT_UNSUPPORTED_ABI;
}

__attribute__((noinline)) int HookedAdd(int left, int right) {
    return g_original_add(left, right) + 100;
}

__attribute__((noinline)) int LocalGateReplacement(int left, int right) {
    g_local_gate_replacement_hits.fetch_add(1, std::memory_order_relaxed);
    return left + right + 40;
}

__attribute__((noinline)) int GeneratedGateReplacement(int left, int right) {
    auto* published = g_generated_gate_hook.load(std::memory_order_acquire);
    if (published == nullptr) return -1000;
    g_generated_gate_entered.store(true, std::memory_order_release);
    while (g_generated_gate_hold.load(std::memory_order_acquire)) std::this_thread::yield();
    const int result = left + right + 40;
    // A real generated callback transfers this short gate pin into
    // HookRecord::in_flight before releasing it. This device-only probe holds
    // the pin through the callback body so DRAINING->CLOSED can be checked
    // against the actual ARM64 LL/SC gate implementation.
    dartplant::ReleasePublishedHostHookEntrant(published);
    return result;
}

__attribute__((noinline)) int GenerationGateReplacement(int left, int right) {
    auto* hook = g_generation_gate_hook.load(std::memory_order_acquire);
    if (hook == nullptr || hook->published_entry_hook == nullptr) return -1000;
    std::vector<std::shared_ptr<dartplant::DartPlantListenerRecord>> listeners;
    if (!dartplant::BeginInvocation(hook, &listeners)) {
        dartplant::ReleasePublishedHostHookEntrant(hook);
        return -1001;
    }
    // A stale runtime generation must suppress logical callbacks while still
    // converting the generated-gate entrant into HookRecord::in_flight. This
    // mirrors the real dispatcher handoff closely enough to prove physical
    // backend teardown cannot race the original body on device.
    dartplant::ReleasePublishedHostHookEntrant(hook);
    auto original = reinterpret_cast<Add>(hook->backup.load(std::memory_order_acquire));
    const int result = original == nullptr ? -1002 : original(left, right);
    dartplant::InvocationExited(hook);
    return listeners.empty() ? result : -1003;
}

int LegacyDobbyHostHook(void*, void* target, void* replacement, void** backup) {
    return DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(replacement),
                     reinterpret_cast<dobby_dummy_func_t*>(backup));
}

int LegacyDobbyHostUnhook(void*, void* target) { return DobbyDestroy(target); }

int ExerciseLegacyDobbyLocalPublicationGate() {
    dartplant::HostApiBinding binding{};
    binding.hook = LegacyDobbyHostHook;
    binding.unhook = LegacyDobbyHostUnhook;
    binding.publication_policy = dartplant::HostPublicationPolicy::kLocalGate;

    // Deliberately retain the cookie/gate after first publication. The
    // production ownership model does the same because a CPU may have fetched
    // the old backend branch before DobbyDestroy restores target bytes.
    auto* published = new dartplant::PublishedHostHook();
    if (dartplant::PreparePublishedHostHook(
            published, &binding, reinterpret_cast<uintptr_t>(DartPlantFixtureAdd),
            reinterpret_cast<void*>(LocalGateReplacement), false) != DARTPLANT_OK) {
        return Fail("legacy Dobby local gate prepare");
    }
    void* backup = nullptr;
    if (dartplant::InstallPublishedHostHook(published, &backup) != DARTPLANT_OK ||
        backup == nullptr) {
        return Fail("legacy Dobby local gate install");
    }

    // The physical Dobby patch is already live, but DartPlant controls logical
    // publication independently. BYPASS_BACKUP must preserve original behavior.
    dartplant::BypassPublishedHostHookToBackup(published);
    if (DartPlantFixtureAdd(2, 3) != 5) {
        return Fail("legacy Dobby local gate pre-arm bypass");
    }
    dartplant::ArmPublishedHostHook(published);
    if (DartPlantFixtureAdd(2, 3) != 45) {
        return Fail("legacy Dobby local gate armed callback");
    }

    dartplant::BeginDrainPublishedHostHook(published);
    if (!dartplant::PublishedHostHookEntrantsIdle(published) ||
        dartplant::UninstallPublishedHostHook(published) != DARTPLANT_OK ||
        DartPlantFixtureAdd(2, 3) != 5) {
        return Fail("legacy Dobby local gate drain/unhook");
    }

    // A stale fetch of the retained DartPlant gate after DobbyDestroy must go
    // to restored target, never the now-backend-owned/reclaimable backup.
    using NativeAdd = int (*)(int, int);
    auto stale_gate = reinterpret_cast<NativeAdd>(published->gate_entry);
    if (stale_gate == nullptr || stale_gate(2, 3) != 5) {
        return Fail("legacy Dobby local gate stale target bypass");
    }

    // Rehook the exact same physical target with a fresh DartPlant gate. A
    // retired gate is deliberately a target alias after physical unhook: it
    // never touches its reclaimed first-generation backup, and while a newer
    // hook owns the target it naturally follows that current publication.
    auto* rehook = new dartplant::PublishedHostHook();
    if (dartplant::PreparePublishedHostHook(
            rehook, &binding, reinterpret_cast<uintptr_t>(DartPlantFixtureAdd),
            reinterpret_cast<void*>(LocalGateReplacement), false) != DARTPLANT_OK) {
        return Fail("legacy Dobby local gate rehook prepare");
    }
    void* rehook_backup = nullptr;
    if (dartplant::InstallPublishedHostHook(rehook, &rehook_backup) != DARTPLANT_OK ||
        rehook_backup == nullptr) {
        return Fail("legacy Dobby local gate rehook install");
    }
    dartplant::ArmPublishedHostHook(rehook);
    if (DartPlantFixtureAdd(2, 3) != 45 || stale_gate(2, 3) != 45) {
        return Fail("legacy Dobby local gate rehook routing");
    }
    dartplant::BeginDrainPublishedHostHook(rehook);
    if (dartplant::UninstallPublishedHostHook(rehook) != DARTPLANT_OK ||
        DartPlantFixtureAdd(2, 3) != 5) {
        return Fail("legacy Dobby local gate rehook unhook");
    }
    auto stale_rehook_gate = reinterpret_cast<NativeAdd>(rehook->gate_entry);
    if (stale_rehook_gate == nullptr || stale_rehook_gate(2, 3) != 5 || stale_gate(2, 3) != 5) {
        return Fail("legacy Dobby local gate rehook stale target bypass");
    }
    return 0;
}

int ExerciseLegacyDobbyConcurrentPublicationGate() {
    dartplant::HostApiBinding binding{};
    binding.hook = LegacyDobbyHostHook;
    binding.unhook = LegacyDobbyHostUnhook;
    binding.publication_policy = dartplant::HostPublicationPolicy::kLocalGate;

    auto* published = new dartplant::PublishedHostHook();
    if (dartplant::PreparePublishedHostHook(
            published, &binding, reinterpret_cast<uintptr_t>(DartPlantFixtureAdd),
            reinterpret_cast<void*>(LocalGateReplacement), false) != DARTPLANT_OK) {
        return Fail("concurrent legacy Dobby gate prepare");
    }

    std::atomic_bool stop{false};
    std::atomic_uint64_t original_results{0};
    std::atomic_uint64_t replacement_results{0};
    std::atomic_uint64_t invalid_results{0};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const int result = DartPlantFixtureAdd(2, 3);
                if (result == 5) {
                    original_results.fetch_add(1, std::memory_order_relaxed);
                } else if (result == 45) {
                    replacement_results.fetch_add(1, std::memory_order_relaxed);
                } else {
                    invalid_results.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    while (original_results.load(std::memory_order_acquire) < 10000) std::this_thread::yield();

    void* backup = nullptr;
    if (dartplant::InstallPublishedHostHook(published, &backup) != DARTPLANT_OK ||
        backup == nullptr) {
        stop.store(true, std::memory_order_release);
        for (auto& worker : workers) worker.join();
        return Fail("concurrent legacy Dobby gate install");
    }
    dartplant::ArmPublishedHostHook(published);
    while (replacement_results.load(std::memory_order_acquire) < 10000) {
        std::this_thread::yield();
    }

    dartplant::BeginDrainPublishedHostHook(published);
    if (dartplant::UninstallPublishedHostHook(published) != DARTPLANT_OK) {
        stop.store(true, std::memory_order_release);
        for (auto& worker : workers) worker.join();
        return Fail("concurrent legacy Dobby gate unhook");
    }
    const uint64_t original_before_restore = original_results.load(std::memory_order_acquire);
    while (original_results.load(std::memory_order_acquire) < original_before_restore + 10000) {
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    if (invalid_results.load(std::memory_order_acquire) != 0 ||
        g_local_gate_replacement_hits.load(std::memory_order_acquire) == 0 ||
        replacement_results.load(std::memory_order_acquire) == 0) {
        return Fail("concurrent legacy Dobby gate routing");
    }
    return 0;
}

int ExerciseGeneratedPublicationGateEntrantDrain() {
    constexpr size_t kFakeStackSize = 1U << 20;
    void* mapping =
        mmap(nullptr, kFakeStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return Fail("map generated publication gate Dart stack");
    const uintptr_t end = reinterpret_cast<uintptr_t>(mapping) + kFakeStackSize;
    const uintptr_t dart_spreg = (end - (64U << 10)) & ~uintptr_t{0xf};

    dartplant::HostApiBinding binding{};
    binding.hook = LegacyDobbyHostHook;
    binding.unhook = LegacyDobbyHostUnhook;
    binding.publication_policy = dartplant::HostPublicationPolicy::kLocalGate;

    // Retain after publication exactly like production PublishedCallbackHooks.
    auto* published = new dartplant::PublishedHostHook();
    if (dartplant::PreparePublishedHostHook(
            published, &binding, reinterpret_cast<uintptr_t>(DartPlantFixtureAdd),
            reinterpret_cast<void*>(GeneratedGateReplacement), true) != DARTPLANT_OK) {
        munmap(mapping, kFakeStackSize);
        return Fail("generated publication gate prepare");
    }
    void* backup = nullptr;
    if (dartplant::InstallPublishedHostHook(published, &backup) != DARTPLANT_OK ||
        backup == nullptr) {
        munmap(mapping, kFakeStackSize);
        return Fail("generated publication gate install");
    }

    g_generated_gate_hook.store(published, std::memory_order_release);
    g_generated_gate_entered.store(false, std::memory_order_release);
    g_generated_gate_hold.store(true, std::memory_order_release);
    dartplant::ArmPublishedHostHook(published);

    int callback_result = 0;
    std::thread entrant([&] {
        callback_result = DartPlantDeviceInvokeGeneratedGate(
            reinterpret_cast<void*>(DartPlantFixtureAdd), dart_spreg, 2, 3);
    });
    while (!g_generated_gate_entered.load(std::memory_order_acquire)) std::this_thread::yield();

    if (dartplant::PublishedHostHookEntrantCount(published) != 1) {
        g_generated_gate_hold.store(false, std::memory_order_release);
        entrant.join();
        munmap(mapping, kFakeStackSize);
        return Fail("generated publication gate entrant pin");
    }

    dartplant::BeginDrainPublishedHostHook(published);
    // The exact ARM64 gate pin and DRAINING/CLOSED state share one 64-bit
    // control word. While this callback owns the high-half entrant count,
    // physical unhook must remain impossible.
    if (dartplant::ClosePublishedHostHook(published) ||
        dartplant::UninstallPublishedHostHook(published) != DARTPLANT_VM_ADAPTER_BUSY) {
        g_generated_gate_hold.store(false, std::memory_order_release);
        entrant.join();
        munmap(mapping, kFakeStackSize);
        return Fail("generated publication gate premature close");
    }

    g_generated_gate_hold.store(false, std::memory_order_release);
    entrant.join();
    if (callback_result != 45 || !dartplant::PublishedHostHookEntrantsIdle(published) ||
        !dartplant::ClosePublishedHostHook(published) ||
        dartplant::UninstallPublishedHostHook(published) != DARTPLANT_OK) {
        munmap(mapping, kFakeStackSize);
        return Fail("generated publication gate drain/unhook");
    }

    // Both an ordinary call through the restored target and a stale fetch of
    // the retained gate must now execute the original target directly without
    // touching the backend trampoline that unhook was allowed to reclaim.
    const int restored = DartPlantDeviceInvokeGeneratedGate(
        reinterpret_cast<void*>(DartPlantFixtureAdd), dart_spreg, 2, 3);
    const int stale = DartPlantDeviceInvokeGeneratedGate(published->gate_entry, dart_spreg, 2, 3);
    g_generated_gate_hook.store(nullptr, std::memory_order_release);
    munmap(mapping, kFakeStackSize);
    if (restored != 5 || stale != 5) return Fail("generated publication gate stale target bypass");
    return 0;
}

int ExerciseGeneratedGateStaleGenerationRetirement() {
    constexpr size_t kFakeStackSize = 1U << 20;
    void* mapping =
        mmap(nullptr, kFakeStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return Fail("map stale-generation Dart stack");
    const uintptr_t end = reinterpret_cast<uintptr_t>(mapping) + kFakeStackSize;
    const uintptr_t dart_spreg = (end - (64U << 10)) & ~uintptr_t{0xf};

    dartplant::HostApiBinding binding{};
    binding.hook = LegacyDobbyHostHook;
    binding.unhook = LegacyDobbyHostUnhook;
    binding.publication_policy = dartplant::HostPublicationPolicy::kLocalGate;

    auto published = std::make_unique<dartplant::PublishedHostHook>();
    if (dartplant::PreparePublishedHostHook(
            published.get(), &binding, reinterpret_cast<uintptr_t>(DartPlantFixtureAdd),
            reinterpret_cast<void*>(GenerationGateReplacement), true) != DARTPLANT_OK) {
        munmap(mapping, kFakeStackSize);
        return Fail("stale-generation publication gate prepare");
    }
    void* backup = nullptr;
    if (dartplant::InstallPublishedHostHook(published.get(), &backup) != DARTPLANT_OK ||
        backup == nullptr) {
        munmap(mapping, kFakeStackSize);
        return Fail("stale-generation publication gate install");
    }

    DartPlantHook hook{};
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.active.store(true, std::memory_order_release);
    hook.host_binding = &binding;
    hook.backend_installed.store(true, std::memory_order_release);
    hook.backup.store(backup, std::memory_order_release);
    hook.code_target = std::make_shared<dartplant::DartEntryTarget>();
    hook.code_target->id = reinterpret_cast<uintptr_t>(DartPlantFixtureAdd);
    hook.code_target->entry = hook.code_target->id;
    hook.runtime_generation = std::make_shared<std::atomic_uint64_t>(7);
    hook.expected_runtime_generation = 7;
    hook.published_entry_hook = std::move(published);
    g_generation_gate_hook.store(&hook, std::memory_order_release);
    dartplant::ArmPublishedHostHook(hook.published_entry_hook.get());

    // Make the logical method stale while its physical entry is still live.
    // A generated entrant must execute the original result (5), with no
    // listener dispatch, and leave both entrant/in_flight counts idle.
    hook.runtime_generation->store(8, std::memory_order_release);
    const int stale_result = DartPlantDeviceInvokeGeneratedGate(
        reinterpret_cast<void*>(DartPlantFixtureAdd), dart_spreg, 2, 3);
    if (stale_result != 5 || hook.in_flight != 0 ||
        !dartplant::PublishedHostHookEntrantsIdle(hook.published_entry_hook.get())) {
        g_generation_gate_hook.store(nullptr, std::memory_order_release);
        munmap(mapping, kFakeStackSize);
        return Fail("stale-generation tracked original");
    }

    // Runtime invalidation ultimately drives the same logical unhook path.
    // Once the tracked original is gone, retirement can close the gate,
    // restore the physical target, and make every stale gate fetch bypass the
    // backend trampoline.
    if (dartplant::RemoveHook(&hook) != DARTPLANT_OK ||
        hook.state != dartplant::HookRecordState::kUnhooked ||
        hook.backend_installed.load(std::memory_order_acquire)) {
        g_generation_gate_hook.store(nullptr, std::memory_order_release);
        munmap(mapping, kFakeStackSize);
        return Fail("stale-generation retirement unhook");
    }
    const int restored = DartPlantDeviceInvokeGeneratedGate(
        reinterpret_cast<void*>(DartPlantFixtureAdd), dart_spreg, 2, 3);
    const int stale_gate =
        DartPlantDeviceInvokeGeneratedGate(hook.published_entry_hook->gate_entry, dart_spreg, 2, 3);
    g_generation_gate_hook.store(nullptr, std::memory_order_release);
    munmap(mapping, kFakeStackSize);
    if (restored != 5 || stale_gate != 5) return Fail("stale-generation target bypass");
    return 0;
}

int Fail(const char* message) {
    std::fprintf(stderr, "[FAIL] %s: %s\n", message, dartplant_last_error());
    return 1;
}

int ExerciseListenerChain(const DartPlantMethod* method,
                          const DartPlantHookOptions& callback_options) {
    DartPlantHookOptions listener_options = callback_options;
    listener_options.on_enter = ObserveEnter;
    listener_options.on_leave = nullptr;
    DartPlantListener* listener = nullptr;
    if (dartplant::AddCallbackListenerForMethod(method, listener_options, 100, &listener) !=
        DARTPLANT_OK) {
        return Fail("public listener chain add");
    }
    if (!dartplant_listener_is_active(listener)) {
        return Fail("public listener chain active");
    }
    if (DartPlantFixtureAdd(2, 3) != 115) {
        return Fail("public listener chain call");
    }
    if (g_listener_calls != 1) {
        return Fail("public listener chain");
    }
    if (dartplant_remove_listener(listener) != DARTPLANT_OK ||
        dartplant_listener_is_active(listener) || !dartplant_listener_is_idle(listener)) {
        return Fail("listener removal and idle state");
    }
    dartplant_release_listener(listener);
    return 0;
}

}  // namespace

int main() {
#if !defined(__aarch64__)
    std::fprintf(stderr, "[FAIL] device test requires ARM64\n");
    return 1;
#endif

    const DartPlantHostApi legacy_dobby_host = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = nullptr,
        .hook = LegacyDobbyHostHook,
        .unhook = LegacyDobbyHostUnhook,
        .hook_with_publication = nullptr,
    };
    dartplant::InstallHostApi(&legacy_dobby_host, dartplant::HostPublicationPolicy::kLocalGate);
    if (ExerciseLegacyDobbyLocalPublicationGate() != 0) return 1;
    if (ExerciseLegacyDobbyConcurrentPublicationGate() != 0) return 1;
    if (ExerciseGeneratedPublicationGateEntrantDrain() != 0) return 1;
    if (ExerciseGeneratedGateStaleGenerationRetirement() != 0) return 1;
    if (ExercisePayloadReturnOwnership() != 0) return 1;
    if (ExerciseDartPadBranch() != 0) return 1;
    dartplant::RefreshModules();

    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(DartPlantFixtureAdd), &info) == 0 ||
        info.dli_fname == nullptr) {
        return Fail("dladdr fixture");
    }
    const std::string path(info.dli_fname);
    const size_t slash = path.find_last_of('/');
    const std::string module_name = slash == std::string::npos ? path : path.substr(slash + 1);

    Dl_info runtime_info{};
    if (dladdr(reinterpret_cast<void*>(dartplant_last_error), &runtime_info) == 0 ||
        runtime_info.dli_fname == nullptr) {
        return Fail("dladdr runtime");
    }
    const std::string runtime_path(runtime_info.dli_fname);
    const size_t runtime_slash = runtime_path.find_last_of('/');
    const std::string runtime_module_name =
        runtime_slash == std::string::npos ? runtime_path : runtime_path.substr(runtime_slash + 1);

    DartPlantAddressQuery query = {
        .struct_size = sizeof(query),
        .module_name = module_name.c_str(),
        .address = reinterpret_cast<uint64_t>(DartPlantFixtureAdd),
        .address_kind = DARTPLANT_ADDRESS_RUNTIME,
        .code_size = 4,
        .expected_build_id = nullptr,
        .expected_fingerprint = nullptr,
    };
    DartPlantHook* hook = nullptr;
    void* backup = nullptr;
    if (dartplant_hook_address(&query, reinterpret_cast<void*>(HookedAdd), &backup, &hook) !=
        DARTPLANT_OK) {
        return Fail("install address hook");
    }
    g_original_add = reinterpret_cast<Add>(backup);
    if (g_original_add == nullptr || DartPlantFixtureAdd(2, 3) != 105) {
        return Fail("replacement and original call");
    }

    DartPlantHook* duplicate = nullptr;
    void* duplicate_backup = nullptr;
    if (dartplant_hook_address(&query, reinterpret_cast<void*>(HookedAdd), &duplicate_backup,
                               &duplicate) != DARTPLANT_ALREADY_HOOKED) {
        return Fail("duplicate hook rejection");
    }

    if (dartplant_unhook(hook) != DARTPLANT_OK || DartPlantFixtureAdd(2, 3) != 5) {
        return Fail("unhook and restore");
    }
    dartplant_release_hook(hook);

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    profile.argument_count = 2;
    profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    profile.argument_locations[1] = {DARTPLANT_ABI_GP_REGISTER, 1, {0, 0}};
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    // This executable is intentionally not a Dart VM. Build a synthetic method
    // only to validate the ARM64 callback/trampoline backend; runtime Function
    // discovery is tested by the real Flutter fixture below this test layer.
    dartplant::DartEntryTargetRegistry entry_targets;
    auto code_target =
        entry_targets.GetOrCreate(reinterpret_cast<uintptr_t>(DartPlantFixtureAdd), 4, 0, 1);
    auto function = std::make_shared<dartplant::DartFunctionHandle>();
    function->identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "add",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    function->source = dartplant::DartFunctionSource::kSynthetic;
    function->code_target = code_target;
    code_target->AddAlias(function->identity);
    DartPlantMethod method{};
    method.record.library_uri = function->identity.library_uri;
    method.record.class_name = function->identity.class_name;
    method.record.function_name = function->identity.function_name;
    method.record.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    method.record.address_kind = DARTPLANT_ADDRESS_RUNTIME;
    method.record.address = reinterpret_cast<uintptr_t>(DartPlantFixtureAdd);
    method.record.code_size = 4;
    method.function = function;

    // A conservative profile can still install a raw callback. No argument
    // mapping is guessed: only the raw ARM64 context API is available.
    DartPlantRuntimeProfile raw_context_profile{};
    dartplant_runtime_profile_init_arm64_aot(&raw_context_profile);
    DartPlantHookOptions callback_options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = ObserveRawContext,
        .on_leave = nullptr,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    g_raw_context_callback_ok = false;
    DartPlantHook* callback_hook = nullptr;
    if (dartplant::InstallCallbackHook(&method, raw_context_profile, callback_options, 0,
                                       &callback_hook, nullptr) != DARTPLANT_OK ||
        DartPlantFixtureAdd(2, 3) != 5 || !g_raw_context_callback_ok) {
        return Fail("raw context callback without ABI mapping");
    }
    if (dartplant_unhook(callback_hook) != DARTPLANT_OK) {
        return Fail("raw context callback unhook");
    }
    dartplant_release_hook(callback_hook);

    callback_options.on_enter = OnEnter;
    callback_options.on_leave = OnLeave;
    g_enter_calls = 0;
    g_leave_calls = 0;
    g_listener_calls = 0;
    callback_hook = nullptr;
    if (dartplant::InstallCallbackHook(&method, profile, callback_options, 0, &callback_hook,
                                       nullptr) != DARTPLANT_OK) {
        return Fail("callback hook install");
    }
    if (g_listener_calls != 0 || DartPlantFixtureAdd(2, 3) != 115 || g_enter_calls != 1 ||
        g_leave_calls != 1) {
        return Fail("callback enter/leave hook");
    }
    if (ExerciseListenerChain(&method, callback_options) != 0) return 1;
    if (dartplant_unhook(callback_hook) != DARTPLANT_OK) {
        return Fail("callback unhook");
    }
    dartplant_release_hook(callback_hook);

    constexpr uint64_t kCanonicalNull = 0x100000001;
    DartPlantRuntimeProfile null_profile = profile;
    null_profile.flags |=
        DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    callback_options.on_enter = SkipWithValidatedNull;
    callback_options.on_leave = nullptr;
    callback_hook = nullptr;
    g_null_semantic_callback_ok = false;
    if (dartplant::InstallCallbackHook(&method, null_profile, callback_options, 0, &callback_hook,
                                       nullptr, kCanonicalNull) != DARTPLANT_OK ||
        DartPlantFixtureAdd(2, 3) != 1 || !g_null_semantic_callback_ok) {
        return Fail("callback validated null semantic hook");
    }
    if (dartplant_unhook(callback_hook) != DARTPLANT_OK) {
        return Fail("validated null callback unhook");
    }
    dartplant_release_hook(callback_hook);

    callback_options.on_enter = SkipWithResult;
    callback_options.on_leave = nullptr;
    callback_hook = nullptr;
    if (dartplant::InstallCallbackHook(&method, profile, callback_options, 0, &callback_hook,
                                       nullptr) != DARTPLANT_OK ||
        DartPlantFixtureAdd(2, 3) != 77) {
        return Fail("callback skip-original hook");
    }
    if (dartplant_unhook(callback_hook) != DARTPLANT_OK) {
        return Fail("skip callback unhook");
    }
    dartplant_release_hook(callback_hook);

    query.address = reinterpret_cast<uint64_t>(&query);
    if (dartplant_hook_address(&query, reinterpret_cast<void*>(HookedAdd), &backup, &hook) !=
        DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE) {
        return Fail("non-executable address rejection");
    }

    std::printf("[PASS] ARM64 deterministic Dart x15 alignment pad\n");
    std::printf("[PASS] ARM64 payload-level sibling RET ownership\n");
    std::printf("[PASS] ARM64 legacy Dobby local publication gate\n");
    std::printf("[PASS] ARM64 legacy Dobby local-gate rehook\n");
    std::printf("[PASS] ARM64 concurrent legacy Dobby publication gate\n");
    std::printf("[PASS] ARM64 generated publication gate entrant drain\n");
    std::printf("[PASS] ARM64 local-gate stale-generation retirement\n");
    std::printf("[PASS] ARM64 Dobby hook/original/unhook\n");
    std::printf("[PASS] ARM64 callback enter/leave/result mutation\n");
    std::printf("[PASS] ARM64 raw callback without ABI mapping\n");
    std::printf("[PASS] ARM64 callback skip-original\n");
    std::printf("[PASS] ARM64 validated null callback semantic\n");
    std::printf("[PASS] duplicate hook rejection\n");
    std::printf("[PASS] executable range validation\n");
    return 0;
}
