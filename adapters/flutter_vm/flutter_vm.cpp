#include "dartplant/adapters/flutter_vm.h"

#include <android/log.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include "core/internal.h"
#include "dart_api_dl.h"
#include "vm/abi/resolver.h"
#include "vm/runtime_profiles.h"

namespace {

struct RealHandle {
    Dart_PersistentHandle strong = nullptr;
    Dart_WeakPersistentHandle weak = nullptr;
    DartPlantObjectStrength strength = DARTPLANT_OBJECT_STRONG;
    std::atomic<bool> alive = true;
    uint32_t root_slot = UINT32_MAX;
    struct State* owner = nullptr;
};

constexpr uint32_t kRootSlotCount = 128;
constexpr uint32_t kLeaseCount = 4;
constexpr uint32_t kRootsPerLease = 96;

struct RootSlot {
    Dart_PersistentHandle handle = nullptr;
    std::atomic_bool used{false};
};

struct RootLease {
    std::atomic_bool used{false};
    uint32_t count = 0;
    std::array<uint16_t, kRootsPerLease> slots{};
    struct State* owner = nullptr;
};

struct State {
    DartPlantVmAdapter* adapter = nullptr;
    DartPlantIsolateIdentity identity{};
    uint64_t thread = 0;
    uint64_t null_raw = 0;
    uint64_t enter_safepoint = 0;
    uint64_t exit_safepoint = 0;
    std::array<RootSlot, kRootSlotCount> roots{};
    std::array<RootLease, kLeaseCount> leases{};
    const dartplant::RuntimeProfileRecord* profile = nullptr;
    dartplant::vm_abi::ArtifactSet artifacts{};
    uint64_t capabilities = dartplant::vm_abi::kCapabilityNone;
    std::atomic<uint64_t> verified_capabilities{dartplant::vm_abi::kCapabilityNone};
    std::atomic<uint64_t> failed_capabilities{dartplant::vm_abi::kCapabilityNone};
    mutable std::mutex artifact_mutex;
    std::atomic_bool artifact_valid{false};
    std::atomic_bool artifact_quiescing{false};
    std::atomic<uint64_t> artifact_generation{1};
    bool isolate_detached = false;
};

struct DescriptorMetadata {
    uint32_t profile_version;
    const char* flutter_version;
    const char* descriptor_id;
};

struct FlutterVmAdapterImpl {
    State state;
};

constexpr DescriptorMetadata kDescriptorMetadata[] = {
    {1, "3.22.3", "flutter-3.22.3-dart-3.4.4-android-arm64-product"},
#if !defined(DARTPLANT_FLUTTER_VM_PROFILE_3_4_4)
    {2, "3.24.0", "flutter-3.24.0-dart-3.5.0-android-arm64-product"},
    {3, "3.44.1", "flutter-3.44.1-dart-3.12.1-android-arm64-product"},
#endif
};

#if defined(DARTPLANT_FLUTTER_VM_PROFILE_3_4_4)
static_assert(std::size(kDescriptorMetadata) == 1,
              "the version-specific Flutter VM adapter must expose only Dart 3.4.4");
#endif

constexpr char kTag[] = "DartPlantFlutterVm";
constexpr uint64_t kEagerRuntimeProofMask =
    dartplant::vm_abi::kCapabilityRuntimeRoots | dartplant::vm_abi::kCapabilityOwnerIdentity |
    dartplant::vm_abi::kCapabilityCanonicalNull | dartplant::vm_abi::kCapabilityRegisterSemantics |
    dartplant::vm_abi::kCapabilityDartCore | dartplant::vm_abi::kCapabilitySafepointStubs;
constexpr uint64_t kIncarnationScopedProofMask =
    dartplant::vm_abi::kCapabilitySafepointStubs |
    dartplant::vm_abi::kCapabilityGeneratedTransitionLayout |
    dartplant::vm_abi::kCapabilityExceptionLayout |
    dartplant::vm_abi::kCapabilityTypeArgumentsLayout |
    dartplant::vm_abi::kCapabilityArtifactLifecycle;
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_RUNTIME_ROOTS_PROVEN) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilityRuntimeRoots));
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_OWNER_IDENTITY_PROVEN) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilityOwnerIdentity));
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_CANONICAL_NULL_PROVEN) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilityCanonicalNull));
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_REGISTER_SEMANTICS_PROVEN) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilityRegisterSemantics));
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_DART_CORE_PROVEN) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilityDartCore));
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_SAFEPOINT_STUBS_PROVEN) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilitySafepointStubs));
static_assert(
    static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_GENERATED_TRANSITION_SOURCE_VERIFIED) ==
    static_cast<uint64_t>(dartplant::vm_abi::kCapabilityGeneratedTransitionLayout));
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_EXCEPTION_SOURCE_VERIFIED) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilityExceptionLayout));
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_TYPE_ARGUMENTS_SOURCE_VERIFIED) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilityTypeArgumentsLayout));
static_assert(static_cast<uint64_t>(DARTPLANT_FLUTTER_VM_CAP_ARTIFACT_LIFECYCLE_BOUND) ==
              static_cast<uint64_t>(dartplant::vm_abi::kCapabilityArtifactLifecycle));
using DartHandlePredicate = bool (*)(Dart_Handle);
DartHandlePredicate g_is_boolean = nullptr;
DartHandlePredicate g_is_integer = nullptr;
DartHandlePredicate g_is_double = nullptr;
DartHandlePredicate g_is_string = nullptr;

extern "C" void dartplant_flutter_vm_call_safepoint_stub(uint64_t thread, uint64_t entry);

uint64_t& ThreadWord(State& state, uintptr_t offset) {
    return *reinterpret_cast<uint64_t*>(state.thread + offset);
}

template <typename T>
bool ReadSelf(uintptr_t address, T* out_value) {
    if (address == 0 || out_value == nullptr) return false;
#if defined(__linux__) && defined(SYS_process_vm_readv)
    iovec local = {.iov_base = out_value, .iov_len = sizeof(T)};
    iovec remote = {.iov_base = reinterpret_cast<void*>(address), .iov_len = sizeof(T)};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(sizeof(T));
#else
    (void) address;
    return false;
#endif
}

uintptr_t CanonicalNativePointer(uint64_t pointer) {
#if defined(__aarch64__)
    return static_cast<uintptr_t>(pointer & 0x00ffffffffffffffULL);
#else
    return static_cast<uintptr_t>(pointer);
#endif
}

bool ReadNativePointer(uintptr_t address, uint64_t* out_pointer) {
    if (out_pointer == nullptr) return false;
    uint64_t raw = 0;
    if (!ReadSelf(address, &raw) || raw == 0) return false;
    *out_pointer = CanonicalNativePointer(raw);
    return *out_pointer != 0;
}

bool IsHeapObject(const dartplant::RuntimeProfileRecord& profile, uint64_t tagged) {
    const auto& raw = profile.raw_object;
    return (tagged & raw.smi_tag_mask) == raw.heap_object_tag && tagged >= raw.heap_object_tag;
}

bool TaggedInHeapWindow(const dartplant::RuntimeProfileRecord& profile, uint64_t heap_base,
                        uint64_t tagged) {
    const auto& raw = profile.raw_object;
    return heap_base != 0 && IsHeapObject(profile, tagged) &&
           heap_base <= UINT64_MAX - raw.heap_object_tag &&
           tagged >= heap_base + raw.heap_object_tag && tagged - heap_base <= UINT32_MAX;
}

DartPlantStatus ValidateCurrentOwner(const State& state) {
    if (state.profile == nullptr || state.thread == 0) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    const Dart_Isolate current_isolate = Dart_CurrentIsolate_DL();
    if (current_isolate == nullptr || CanonicalNativePointer(reinterpret_cast<uint64_t>(
                                          current_isolate)) != state.identity.isolate) {
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    uint64_t thread_isolate = 0;
    uint64_t isolate_group = 0;
    if (!ReadNativePointer(state.thread + state.profile->live_vm.thread_isolate_offset,
                           &thread_isolate) ||
        !ReadNativePointer(state.thread + state.profile->live_vm.thread_isolate_group_offset,
                           &isolate_group)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (thread_isolate != state.identity.isolate || isolate_group != state.identity.isolate_group) {
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    return DARTPLANT_OK;
}

bool ValidateArtifactLifecycle(State& state) {
    if ((state.capabilities & dartplant::vm_abi::kCapabilityArtifactLifecycle) == 0) {
        state.artifact_valid.store(false, std::memory_order_release);
        return false;
    }
    const bool valid =
        dartplant::vm_abi::ValidateArtifactSet(state.artifacts, dartplant::EnumerateModules());
    state.artifact_valid.store(valid, std::memory_order_release);
    return valid;
}

bool HasCapability(const State& state, uint64_t capability) {
    return (state.capabilities & capability) == capability;
}

bool HasVerifiedCapability(const State& state, uint64_t capability) {
    return (state.verified_capabilities.load(std::memory_order_acquire) & capability) == capability;
}

bool HasFailedCapability(const State& state, uint64_t capability) {
    return (state.failed_capabilities.load(std::memory_order_acquire) & capability) != 0;
}

void MarkCapabilityVerified(State& state, uint64_t capability) {
    state.failed_capabilities.fetch_and(~capability, std::memory_order_acq_rel);
    state.verified_capabilities.fetch_or(capability, std::memory_order_acq_rel);
}

void MarkCapabilityFailed(State& state, uint64_t capability) {
    state.verified_capabilities.fetch_and(~capability, std::memory_order_acq_rel);
    state.failed_capabilities.fetch_or(capability, std::memory_order_acq_rel);
}

void ResetCapabilityProof(State& state, uint64_t capability) {
    state.verified_capabilities.fetch_and(~capability, std::memory_order_acq_rel);
    state.failed_capabilities.fetch_and(~capability, std::memory_order_acq_rel);
}

void LogCapabilityTransition(State& state, uint64_t capability, const char* name,
                             const char* result) {
    const uint64_t verified = state.verified_capabilities.load(std::memory_order_acquire);
    const uint64_t failed = state.failed_capabilities.load(std::memory_order_acquire);
    const uint64_t artifact_generation = state.artifact_generation.load(std::memory_order_acquire);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "capability proof name=%s result=%s capability=0x%llx verified=0x%llx failed=0x%llx "
        "artifact_generation=%llu",
        name, result, static_cast<unsigned long long>(capability),
        static_cast<unsigned long long>(verified), static_cast<unsigned long long>(failed),
        static_cast<unsigned long long>(artifact_generation));
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "DARTPLANT_CI {\"event\":\"capability\",\"name\":\"%s\",\"state\":\"%s\","
                        "\"capability\":\"0x%llx\",\"verified\":\"0x%llx\",\"failed\":\"0x%llx\","
                        "\"artifact_generation\":%llu,\"isolate_generation\":%llu}",
                        name, result, static_cast<unsigned long long>(capability),
                        static_cast<unsigned long long>(verified),
                        static_cast<unsigned long long>(failed),
                        static_cast<unsigned long long>(artifact_generation),
                        static_cast<unsigned long long>(state.identity.generation));
}

bool ProveTransitionCapability(State& state) {
    constexpr uint64_t capability = dartplant::vm_abi::kCapabilityGeneratedTransitionLayout;
    if (!HasCapability(state, capability) || state.profile == nullptr) return false;
    const auto proof =
        dartplant::vm_abi::ProveGeneratedTransitionState(*state.profile, state.thread);
    if (!proof.passed) {
        MarkCapabilityFailed(state, capability);
        __android_log_print(
            ANDROID_LOG_WARN, kTag,
            "transition state proof rejected execution=%llu top_exit=0x%llx vm_tag=0x%llx "
            "exit_marker=%llu",
            static_cast<unsigned long long>(proof.execution_state),
            static_cast<unsigned long long>(proof.top_exit_frame),
            static_cast<unsigned long long>(proof.vm_tag),
            static_cast<unsigned long long>(proof.exit_through_ffi));
        LogCapabilityTransition(state, capability, "generated-transition", "failed");
        return false;
    }
    const bool first = !HasVerifiedCapability(state, capability);
    MarkCapabilityVerified(state, capability);
    if (first) {
        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "transition state proof passed execution=%llu top_exit=0x%llx vm_tag=0x%llx "
            "exit_marker=%llu",
            static_cast<unsigned long long>(proof.execution_state),
            static_cast<unsigned long long>(proof.top_exit_frame),
            static_cast<unsigned long long>(proof.vm_tag),
            static_cast<unsigned long long>(proof.exit_through_ffi));
        LogCapabilityTransition(state, capability, "generated-transition", "verified");
    }
    return true;
}

bool ValidateHotArtifactBinding(const State& state) {
    if (!state.artifact_valid.load(std::memory_order_acquire) || state.artifacts.app.path.empty() ||
        state.artifacts.app.load_bias == 0 || state.enter_safepoint == 0 ||
        state.exit_safepoint == 0) {
        return false;
    }
    for (uint64_t entry : {state.enter_safepoint, state.exit_safepoint}) {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(entry), &info) == 0 || info.dli_fname == nullptr ||
            info.dli_fbase == nullptr || state.artifacts.app.path != info.dli_fname ||
            reinterpret_cast<uintptr_t>(info.dli_fbase) != state.artifacts.app.load_bias) {
            return false;
        }
    }
    return true;
}

uint64_t InvalidateArtifactBindingLocked(State& state) {
    state.artifact_valid.store(false, std::memory_order_release);
    ResetCapabilityProof(state, kIncarnationScopedProofMask);
    return state.artifact_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool ValidActiveObjectRaw(const State& state, uint64_t raw, bool allow_smi, bool allow_null) {
    if (raw == state.null_raw) return allow_null;
    const auto& object = state.profile->raw_object;
    const uint64_t tag = raw & object.smi_tag_mask;
    if (tag != object.heap_object_tag) return tag == object.smi_tag && allow_smi;
    uint64_t heap_base = 0;
    if (!ReadSelf(state.thread + state.profile->live_vm.thread_heap_base_offset, &heap_base) ||
        !TaggedInHeapWindow(*state.profile, heap_base, raw)) {
        return false;
    }
    uint64_t tags = 0;
    return ReadSelf(static_cast<uintptr_t>(raw - object.heap_object_tag), &tags);
}

DartPlantStatus ReadAndProveActiveExceptionState(State& state, uint64_t* out_exception,
                                                 uint64_t* out_stacktrace) {
    constexpr uint64_t capability = dartplant::vm_abi::kCapabilityExceptionLayout;
    if (out_exception == nullptr || out_stacktrace == nullptr ||
        !HasCapability(state, capability) || HasFailedCapability(state, capability)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const DartPlantStatus owner = ValidateCurrentOwner(state);
    if (owner != DARTPLANT_OK) return owner;
    uint64_t exception = 0;
    uint64_t stacktrace = 0;
    if (!ReadSelf(state.thread + state.profile->thread_bridge.active_exception_offset,
                  &exception) ||
        !ReadSelf(state.thread + state.profile->thread_bridge.active_stacktrace_offset,
                  &stacktrace) ||
        !ValidActiveObjectRaw(state, exception, true, false) ||
        !ValidActiveObjectRaw(state, stacktrace, false, true)) {
        const bool first_failure = !HasFailedCapability(state, capability);
        MarkCapabilityFailed(state, capability);
        if (first_failure) {
            LogCapabilityTransition(state, capability, "active-exception", "failed");
        }
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    const bool first_verification = !HasVerifiedCapability(state, capability);
    MarkCapabilityVerified(state, capability);
    if (first_verification) {
        LogCapabilityTransition(state, capability, "active-exception", "verified");
    }
    *out_exception = exception;
    *out_stacktrace = stacktrace;
    return DARTPLANT_OK;
}

uint64_t RootRaw(const RootSlot& slot) { return *reinterpret_cast<const uint64_t*>(slot.handle); }

void SetRootRaw(RootSlot& slot, uint64_t raw) { *reinterpret_cast<uint64_t*>(slot.handle) = raw; }

bool AcquireRoot(State& state, uint32_t* out_index) {
    if (out_index == nullptr) return false;
    for (uint32_t index = 0; index < state.roots.size(); ++index) {
        bool expected = false;
        if (state.roots[index].used.compare_exchange_strong(expected, true,
                                                            std::memory_order_acq_rel)) {
            *out_index = index;
            return true;
        }
    }
    return false;
}

void ReleaseRoot(State& state, uint32_t index) {
    if (index >= state.roots.size()) return;
    SetRootRaw(state.roots[index], state.null_raw);
    state.roots[index].used.store(false, std::memory_order_release);
}

void WeakFinalizer(void*, void* peer) {
    if (peer != nullptr) static_cast<RealHandle*>(peer)->alive.store(false);
}

DartPlantStatus EnterIsolate(void*, const DartPlantIsolateIdentity* identity) {
    if (Dart_CurrentIsolate_DL() != nullptr) return DARTPLANT_VM_ADAPTER_BUSY;
    Dart_EnterIsolate_DL(reinterpret_cast<Dart_Isolate>(identity->isolate));
    if (Dart_CurrentIsolate_DL() != reinterpret_cast<Dart_Isolate>(identity->isolate)) {
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    return DARTPLANT_OK;
}

DartPlantStatus LeaveIsolate(void*, const DartPlantIsolateIdentity*) {
    if (Dart_CurrentIsolate_DL() == nullptr) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    Dart_ExitIsolate_DL();
    return DARTPLANT_OK;
}

DartPlantStatus EnterScope(void*, const DartPlantIsolateIdentity*) {
    if (Dart_CurrentIsolate_DL() == nullptr) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    Dart_EnterScope_DL();
    return DARTPLANT_OK;
}

DartPlantStatus LeaveScope(void*, const DartPlantIsolateIdentity*) {
    if (Dart_CurrentIsolate_DL() == nullptr) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    Dart_ExitScope_DL();
    return DARTPLANT_OK;
}

DartPlantStatus RetainObject(void* user_data, const DartPlantIsolateIdentity*, uint64_t raw,
                             DartPlantObjectStrength strength, void** out_backend) {
    if (out_backend == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    uint32_t slot_index = UINT32_MAX;
    if (!AcquireRoot(*state, &slot_index)) return DARTPLANT_VM_ADAPTER_BUSY;
    RootSlot& slot = state->roots[slot_index];
    SetRootRaw(slot, raw);
    Dart_Handle local = Dart_HandleFromPersistent_DL(slot.handle);
    if (local == nullptr) {
        ReleaseRoot(*state, slot_index);
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    auto* handle = new RealHandle;
    handle->strength = strength;
    handle->owner = state;
    if (strength == DARTPLANT_OBJECT_WEAK) {
        handle->weak = Dart_NewWeakPersistentHandle_DL(local, handle, 0, WeakFinalizer);
        ReleaseRoot(*state, slot_index);
        if (handle->weak == nullptr) {
            delete handle;
            return DARTPLANT_OBJECT_HANDLE_INVALID;
        }
    } else {
        handle->strong = slot.handle;
        handle->root_slot = slot_index;
    }
    *out_backend = handle;
    return DARTPLANT_OK;
}

DartPlantStatus ReleaseObject(void*, const DartPlantIsolateIdentity*, void* backend,
                              DartPlantObjectStrength) {
    auto* handle = static_cast<RealHandle*>(backend);
    if (handle == nullptr) return DARTPLANT_OBJECT_HANDLE_INVALID;
    if (handle->strength == DARTPLANT_OBJECT_WEAK) {
        Dart_DeleteWeakPersistentHandle_DL(handle->weak);
    } else {
        if (handle->owner == nullptr) return DARTPLANT_OBJECT_HANDLE_INVALID;
        ReleaseRoot(*handle->owner, handle->root_slot);
    }
    delete handle;
    return DARTPLANT_OK;
}

DartPlantStatus ObjectKind(void*, const DartPlantIsolateIdentity*, void* backend,
                           DartPlantObjectKind* out_kind) {
    if (backend == nullptr || out_kind == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    auto* handle = static_cast<RealHandle*>(backend);
    Dart_Handle local = handle->strength == DARTPLANT_OBJECT_WEAK
                            ? Dart_HandleFromWeakPersistent_DL(handle->weak)
                            : Dart_HandleFromPersistent_DL(handle->strong);
    if (local == nullptr || (handle->strength == DARTPLANT_OBJECT_WEAK && Dart_IsNull_DL(local))) {
        return DARTPLANT_OBJECT_COLLECTED;
    }
    if (Dart_IsNull_DL(local)) {
        *out_kind = DARTPLANT_OBJECT_NULL;
    } else if (g_is_boolean != nullptr && g_is_boolean(local)) {
        *out_kind = DARTPLANT_OBJECT_BOOL;
    } else if (g_is_integer != nullptr && g_is_integer(local)) {
        *out_kind = DARTPLANT_OBJECT_SMI;
    } else if (g_is_double != nullptr && g_is_double(local)) {
        *out_kind = DARTPLANT_OBJECT_DOUBLE;
    } else if (g_is_string != nullptr && g_is_string(local)) {
        *out_kind = DARTPLANT_OBJECT_STRING;
    } else {
        *out_kind = DARTPLANT_OBJECT_OTHER;
    }
    return DARTPLANT_OK;
}

DartPlantStatus ObjectToRaw(void*, const DartPlantIsolateIdentity*, void* backend,
                            uint64_t* out_raw) {
    if (backend == nullptr || out_raw == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    auto* handle = static_cast<RealHandle*>(backend);
    if (handle->strength == DARTPLANT_OBJECT_STRONG) {
        if (handle->owner == nullptr || handle->root_slot >= handle->owner->roots.size()) {
            return DARTPLANT_OBJECT_HANDLE_INVALID;
        }
        *out_raw = RootRaw(handle->owner->roots[handle->root_slot]);
        return DARTPLANT_OK;
    }
    Dart_Handle local = Dart_HandleFromWeakPersistent_DL(handle->weak);
    if (local == nullptr || Dart_IsNull_DL(local)) return DARTPLANT_OBJECT_COLLECTED;
    Dart_PersistentHandle temporary = Dart_NewPersistentHandle_DL(local);
    if (temporary == nullptr) return DARTPLANT_OBJECT_HANDLE_INVALID;
    *out_raw = *reinterpret_cast<const uint64_t*>(temporary);
    Dart_DeletePersistentHandle_DL(temporary);
    return DARTPLANT_OK;
}

DartPlantStatus ObjectAlive(void*, const DartPlantIsolateIdentity*, void* backend,
                            uint8_t* out_alive) {
    if (backend == nullptr || out_alive == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    auto* handle = static_cast<RealHandle*>(backend);
    if (handle->strength == DARTPLANT_OBJECT_WEAK && handle->weak != nullptr) {
        Dart_Handle local = Dart_HandleFromWeakPersistent_DL(handle->weak);
        *out_alive = local != nullptr && !Dart_IsNull_DL(local) ? 1 : 0;
    } else {
        *out_alive = handle->alive.load() ? 1 : 0;
    }
    return DARTPLANT_OK;
}

bool SelfTestPersistentApi(uint64_t* out_null_raw) {
    if (out_null_raw == nullptr) return false;
    Dart_EnterScope_DL();
    Dart_Handle null_handle = Dart_Null_DL();
    Dart_PersistentHandle strong = Dart_NewPersistentHandle_DL(null_handle);
    Dart_WeakPersistentHandle weak =
        Dart_NewWeakPersistentHandle_DL(null_handle, nullptr, 0, WeakFinalizer);
    const bool valid = strong != nullptr && weak != nullptr;
    if (strong != nullptr) *out_null_raw = *reinterpret_cast<const uint64_t*>(strong);
    if (weak != nullptr) Dart_DeleteWeakPersistentHandle_DL(weak);
    if (strong != nullptr) Dart_DeletePersistentHandle_DL(strong);
    Dart_ExitScope_DL();
    return valid;
}

void DeleteRoots(State& state) {
    for (auto& root : state.roots) {
        if (root.handle != nullptr) Dart_DeletePersistentHandle_DL(root.handle);
        root.handle = nullptr;
        root.used.store(false, std::memory_order_release);
    }
}

DartPlantStatus PinGeneratedRoots(void* user_data, const DartPlantIsolateIdentity* identity,
                                  const uint64_t* raw_values, uint32_t value_count,
                                  void** out_root_lease) {
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr || identity == nullptr || raw_values == nullptr ||
        out_root_lease == nullptr || value_count == 0 || value_count > kRootsPerLease ||
        identity->isolate != state->identity.isolate ||
        identity->isolate_group != state->identity.isolate_group) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    RootLease* lease = nullptr;
    for (auto& candidate : state->leases) {
        bool expected = false;
        if (candidate.used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            lease = &candidate;
            break;
        }
    }
    if (lease == nullptr) return DARTPLANT_VM_ADAPTER_BUSY;
    lease->owner = state;
    lease->count = 0;
    for (uint32_t index = 0; index < value_count; ++index) {
        uint32_t root = UINT32_MAX;
        if (!AcquireRoot(*state, &root)) {
            for (uint32_t cursor = 0; cursor < lease->count; ++cursor) {
                ReleaseRoot(*state, lease->slots[cursor]);
            }
            lease->used.store(false, std::memory_order_release);
            return DARTPLANT_VM_ADAPTER_BUSY;
        }
        lease->slots[index] = static_cast<uint16_t>(root);
        SetRootRaw(state->roots[root], raw_values[index]);
        ++lease->count;
    }
    std::atomic_thread_fence(std::memory_order_release);
    *out_root_lease = lease;
    return DARTPLANT_OK;
}

DartPlantStatus GeneratedRootGet(void*, const DartPlantIsolateIdentity*, void* root_lease,
                                 uint32_t index, uint64_t* out_raw) {
    auto* lease = static_cast<RootLease*>(root_lease);
    if (lease == nullptr || out_raw == nullptr || !lease->used.load(std::memory_order_acquire) ||
        lease->owner == nullptr || index >= lease->count) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_raw = RootRaw(lease->owner->roots[lease->slots[index]]);
    return DARTPLANT_OK;
}

DartPlantStatus GeneratedRootSet(void*, const DartPlantIsolateIdentity*, void* root_lease,
                                 uint32_t index, uint64_t raw) {
    auto* lease = static_cast<RootLease*>(root_lease);
    if (lease == nullptr || !lease->used.load(std::memory_order_acquire) ||
        lease->owner == nullptr || index >= lease->count) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    SetRootRaw(lease->owner->roots[lease->slots[index]], raw);
    return DARTPLANT_OK;
}

DartPlantStatus UnpinGeneratedRoots(void*, const DartPlantIsolateIdentity*, void* root_lease,
                                    uint64_t* out_raw_values, uint32_t value_count) {
    auto* lease = static_cast<RootLease*>(root_lease);
    if (lease == nullptr || out_raw_values == nullptr ||
        !lease->used.load(std::memory_order_acquire) || lease->owner == nullptr ||
        value_count != lease->count) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < lease->count; ++index) {
        out_raw_values[index] = RootRaw(lease->owner->roots[lease->slots[index]]);
        ReleaseRoot(*lease->owner, lease->slots[index]);
    }
    lease->count = 0;
    lease->used.store(false, std::memory_order_release);
    return DARTPLANT_OK;
}

DartPlantStatus EnterGeneratedToNative(void* user_data, const DartPlantIsolateIdentity* identity,
                                       const DartPlantGeneratedTransitionFrame* frame, void*) {
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr || identity == nullptr || frame == nullptr || state->profile == nullptr ||
        frame->thread != state->thread || identity->isolate != state->identity.isolate ||
        (frame->flags & DARTPLANT_GENERATED_TRANSITION_SYNTHETIC_EXIT_FRAME) == 0) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    // Serialize the validated artifact identity with every write + safepoint
    // stub call. Immediate invalidation therefore cannot return while a thread
    // is still inside old-incarnation executable code.
    std::lock_guard artifact_lock(state->artifact_mutex);
    if (!HasCapability(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout |
                                   dartplant::vm_abi::kCapabilitySafepointStubs |
                                   dartplant::vm_abi::kCapabilityArtifactLifecycle)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (HasFailedCapability(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!HasVerifiedCapability(*state, dartplant::vm_abi::kCapabilitySafepointStubs |
                                           dartplant::vm_abi::kCapabilityArtifactLifecycle)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!ValidateHotArtifactBinding(*state)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "generated transition rejected: VM artifact incarnation changed");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!HasVerifiedCapability(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout) &&
        !ProveTransitionCapability(*state)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const auto& bridge = state->profile->thread_bridge;
    const auto& transition = state->profile->transition;
    const uint64_t execution_state = ThreadWord(*state, bridge.execution_state_offset);
    const uint64_t top_exit_frame = ThreadWord(*state, bridge.top_exit_frame_offset);
    uint64_t& exit_through_ffi = ThreadWord(*state, bridge.exit_through_ffi_offset);
    const uint64_t previous_exit_marker = exit_through_ffi;
    const uint64_t vm_tag = ThreadWord(*state, bridge.vm_tag_offset);
    if (execution_state != transition.execution_generated ||
        top_exit_frame != transition.exit_none || vm_tag != transition.vm_tag_dart) {
        MarkCapabilityFailed(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout);
        LogCapabilityTransition(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout,
                                "generated-transition", "failed-state");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    // A Dart runtime call marks Thread::exit_through_ffi with
    // kExitThroughRuntimeCall and clears it only in the normal
    // CallToRuntimeStub epilogue. Exceptions::JumpToFrame performs a non-local
    // transfer that destroys TransitionGeneratedToVM and returns execution to
    // generated Dart, but both JumpToFrame and RunExceptionHandler bypass that
    // epilogue. The exact 3.4.4 VM therefore can reach a handler with all
    // generated-state invariants restored except for this stale marker. It is
    // safe to normalize only that state: a live runtime/FFI exit still has a
    // non-generated execution state and/or a non-zero top exit frame and is
    // rejected above.
    if (exit_through_ffi == transition.exit_through_runtime_call) {
        exit_through_ffi = transition.exit_none;
    } else if (exit_through_ffi != transition.exit_none) {
        MarkCapabilityFailed(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout);
        LogCapabilityTransition(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout,
                                "generated-transition", "failed-exit-marker");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    ThreadWord(*state, bridge.top_exit_frame_offset) = frame->exit_frame;
    exit_through_ffi = transition.exit_through_ffi;
    ThreadWord(*state, bridge.vm_tag_offset) = reinterpret_cast<uint64_t>(&EnterGeneratedToNative);
    ThreadWord(*state, bridge.execution_state_offset) = transition.execution_native;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    dartplant_flutter_vm_call_safepoint_stub(state->thread, state->enter_safepoint);
    if (ThreadWord(*state, bridge.execution_state_offset) != transition.execution_native ||
        ThreadWord(*state, bridge.top_exit_frame_offset) != frame->exit_frame ||
        ThreadWord(*state, bridge.exit_through_ffi_offset) != transition.exit_through_ffi ||
        ThreadWord(*state, bridge.vm_tag_offset) !=
            reinterpret_cast<uint64_t>(&EnterGeneratedToNative)) {
        ThreadWord(*state, bridge.vm_tag_offset) = vm_tag;
        ThreadWord(*state, bridge.execution_state_offset) = execution_state;
        ThreadWord(*state, bridge.top_exit_frame_offset) = top_exit_frame;
        ThreadWord(*state, bridge.exit_through_ffi_offset) = previous_exit_marker;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        MarkCapabilityFailed(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout);
        LogCapabilityTransition(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout,
                                "generated-transition", "failed-post-enter");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    return DARTPLANT_OK;
}

DartPlantStatus LeaveNativeToGenerated(void* user_data, const DartPlantIsolateIdentity* identity,
                                       const DartPlantGeneratedTransitionFrame* frame, void*) {
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr || identity == nullptr || frame == nullptr || state->profile == nullptr ||
        frame->thread != state->thread) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    std::lock_guard artifact_lock(state->artifact_mutex);
    if (!HasCapability(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout |
                                   dartplant::vm_abi::kCapabilitySafepointStubs |
                                   dartplant::vm_abi::kCapabilityArtifactLifecycle)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!HasVerifiedCapability(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout |
                                           dartplant::vm_abi::kCapabilitySafepointStubs |
                                           dartplant::vm_abi::kCapabilityArtifactLifecycle) ||
        HasFailedCapability(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!ValidateHotArtifactBinding(*state)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "native return rejected: VM artifact incarnation changed");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const auto& bridge = state->profile->thread_bridge;
    const auto& transition = state->profile->transition;
    if (ThreadWord(*state, bridge.execution_state_offset) != transition.execution_native ||
        ThreadWord(*state, bridge.top_exit_frame_offset) != frame->exit_frame ||
        ThreadWord(*state, bridge.exit_through_ffi_offset) != transition.exit_through_ffi) {
        MarkCapabilityFailed(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout);
        LogCapabilityTransition(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout,
                                "generated-transition", "failed-native-state");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const uint64_t previous_vm_tag = ThreadWord(*state, bridge.vm_tag_offset);
    const uint64_t previous_execution_state = ThreadWord(*state, bridge.execution_state_offset);
    const uint64_t previous_top_exit_frame = ThreadWord(*state, bridge.top_exit_frame_offset);
    const uint64_t previous_exit_marker = ThreadWord(*state, bridge.exit_through_ffi_offset);
    dartplant_flutter_vm_call_safepoint_stub(state->thread, state->exit_safepoint);
    ThreadWord(*state, bridge.vm_tag_offset) = transition.vm_tag_dart;
    ThreadWord(*state, bridge.execution_state_offset) = transition.execution_generated;
    ThreadWord(*state, bridge.top_exit_frame_offset) = transition.exit_none;
    ThreadWord(*state, bridge.exit_through_ffi_offset) = transition.exit_none;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (ThreadWord(*state, bridge.vm_tag_offset) != transition.vm_tag_dart ||
        ThreadWord(*state, bridge.execution_state_offset) != transition.execution_generated ||
        ThreadWord(*state, bridge.top_exit_frame_offset) != transition.exit_none ||
        ThreadWord(*state, bridge.exit_through_ffi_offset) != transition.exit_none) {
        ThreadWord(*state, bridge.vm_tag_offset) = previous_vm_tag;
        ThreadWord(*state, bridge.execution_state_offset) = previous_execution_state;
        ThreadWord(*state, bridge.top_exit_frame_offset) = previous_top_exit_frame;
        ThreadWord(*state, bridge.exit_through_ffi_offset) = previous_exit_marker;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        MarkCapabilityFailed(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout);
        LogCapabilityTransition(*state, dartplant::vm_abi::kCapabilityGeneratedTransitionLayout,
                                "generated-transition", "failed-post-leave");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    return DARTPLANT_OK;
}

DartPlantStatus ReadActiveException(void* user_data, const DartPlantIsolateIdentity* identity,
                                    uint64_t* out_raw) {
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr || identity == nullptr || out_raw == nullptr ||
        identity->isolate != state->identity.isolate ||
        identity->isolate_group != state->identity.isolate_group ||
        identity->generation != state->identity.generation || state->profile == nullptr) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    uint64_t exception = 0;
    uint64_t stacktrace = 0;
    const DartPlantStatus status =
        ReadAndProveActiveExceptionState(*state, &exception, &stacktrace);
    if (status != DARTPLANT_OK) return status;
    *out_raw = exception;
    return status;
}

DartPlantStatus ReadActiveStacktrace(void* user_data, const DartPlantIsolateIdentity* identity,
                                     uint64_t* out_raw) {
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr || identity == nullptr || out_raw == nullptr ||
        identity->isolate != state->identity.isolate ||
        identity->isolate_group != state->identity.isolate_group ||
        identity->generation != state->identity.generation || state->profile == nullptr) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    uint64_t exception = 0;
    uint64_t stacktrace = 0;
    const DartPlantStatus status =
        ReadAndProveActiveExceptionState(*state, &exception, &stacktrace);
    if (status != DARTPLANT_OK) return status;
    *out_raw = stacktrace;
    return status;
}

DartPlantStatus ReadTypeArgumentsElement(void* user_data, const DartPlantIsolateIdentity* identity,
                                         uint64_t type_arguments_raw, uint32_t index,
                                         uint64_t* out_raw) {
    auto* state = static_cast<State*>(user_data);
    const auto* type_arguments =
        state == nullptr || state->profile == nullptr ? nullptr : &state->profile->type_arguments;
    const auto* raw =
        state == nullptr || state->profile == nullptr ? nullptr : &state->profile->raw_object;
    if (state == nullptr || identity == nullptr || out_raw == nullptr ||
        type_arguments == nullptr || raw == nullptr ||
        identity->isolate != state->identity.isolate ||
        identity->isolate_group != state->identity.isolate_group ||
        identity->generation != state->identity.generation ||
        (type_arguments_raw & raw->smi_tag_mask) != raw->heap_object_tag) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    constexpr uint64_t capability = dartplant::vm_abi::kCapabilityTypeArgumentsLayout;
    if (!HasCapability(*state, capability) || HasFailedCapability(*state, capability)) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const auto fail_proof = [&](DartPlantStatus status) {
        const bool first_failure = !HasFailedCapability(*state, capability);
        MarkCapabilityFailed(*state, capability);
        if (first_failure) {
            LogCapabilityTransition(*state, capability, "type-arguments-element", "failed");
        }
        return status;
    };
    // This callback is invoked by dartplant_core before Generated->Native and
    // before any Dart API scope/safepoint. The mutator therefore cannot move
    // this object while these raw compressed fields are inspected. Every
    // returned element is immediately copied into the generated-root lease;
    // user callbacks never retain or dereference this object address.
    const uint64_t heap_base = ThreadWord(*state, state->profile->live_vm.thread_heap_base_offset);
    const uintptr_t tagged_object = static_cast<uintptr_t>(type_arguments_raw);
    // Generated Dart registers/stack slots contain a full tagged ObjectPtr even
    // in compressed-pointer builds. Only fields inside heap objects are stored
    // as 32-bit compressed pointers. Constrain the full pointer to this heap's
    // 4-GiB compression window before reading its header.
    if (!TaggedInHeapWindow(*state->profile, heap_base, tagged_object)) {
        return fail_proof(DARTPLANT_OBJECT_HANDLE_INVALID);
    }
    const uintptr_t object = tagged_object - raw->heap_object_tag;

    uint64_t tags = 0;
    if (!ReadSelf(object, &tags)) return fail_proof(DARTPLANT_OBJECT_HANDLE_INVALID);
    if (raw->class_id_tag_bits == 0 || raw->class_id_tag_bits >= 64) {
        return fail_proof(DARTPLANT_PROFILE_MISMATCH);
    }
    const uint64_t class_id_mask = (uint64_t{1} << raw->class_id_tag_bits) - 1;
    const uint32_t cid = static_cast<uint32_t>((tags >> raw->class_id_tag_shift) & class_id_mask);
    if (cid != type_arguments->cid) return fail_proof(DARTPLANT_OBJECT_HANDLE_INVALID);

    uint32_t length_raw = 0;
    if (!ReadSelf(object + type_arguments->length_offset, &length_raw)) {
        return fail_proof(DARTPLANT_OBJECT_HANDLE_INVALID);
    }
    if ((length_raw & raw->smi_tag_mask) != raw->smi_tag) {
        return fail_proof(DARTPLANT_OBJECT_HANDLE_INVALID);
    }
    if ((length_raw >> raw->smi_tag_shift) <= index) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (raw->compressed_word_size != sizeof(uint32_t) ||
        index > (std::numeric_limits<uintptr_t>::max() - type_arguments->types_offset) /
                    sizeof(uint32_t)) {
        return fail_proof(DARTPLANT_PROFILE_MISMATCH);
    }
    uint32_t compressed_element = 0;
    const uintptr_t element_address =
        object + type_arguments->types_offset + static_cast<uintptr_t>(index) * sizeof(uint32_t);
    if (!ReadSelf(element_address, &compressed_element)) {
        return fail_proof(DARTPLANT_OBJECT_HANDLE_INVALID);
    }
    if ((compressed_element & raw->smi_tag_mask) != raw->heap_object_tag) {
        return fail_proof(DARTPLANT_OBJECT_HANDLE_INVALID);
    }
    if (heap_base > std::numeric_limits<uintptr_t>::max() - compressed_element) {
        return fail_proof(DARTPLANT_OBJECT_HANDLE_INVALID);
    }
    *out_raw = heap_base + compressed_element;
    const bool first_verification = !HasVerifiedCapability(*state, capability);
    MarkCapabilityVerified(*state, capability);
    if (first_verification) {
        LogCapabilityTransition(*state, capability, "type-arguments-element", "verified");
    }
    return DARTPLANT_OK;
}

const DartPlantVmAdapterCallbacks kCallbacks = {
    .struct_size = sizeof(DartPlantVmAdapterCallbacks),
    .adapter_version = 3,
    .enter_isolate = EnterIsolate,
    .leave_isolate = LeaveIsolate,
    .enter_scope = EnterScope,
    .leave_scope = LeaveScope,
    .retain_object = RetainObject,
    .release_object = ReleaseObject,
    .object_kind = ObjectKind,
    .object_to_raw = ObjectToRaw,
    .object_is_alive = ObjectAlive,
    .pin_generated_roots = PinGeneratedRoots,
    .generated_root_get = GeneratedRootGet,
    .generated_root_set = GeneratedRootSet,
    .unpin_generated_roots = UnpinGeneratedRoots,
    .enter_generated_to_native = EnterGeneratedToNative,
    .leave_native_to_generated = LeaveNativeToGenerated,
    .read_active_exception = ReadActiveException,
    .read_active_stacktrace = ReadActiveStacktrace,
    .read_type_arguments_element = ReadTypeArgumentsElement,
};

}  // namespace

DartPlantStatus dartplant_flutter_vm_adapter_create(const DartPlantFlutterVmAdapterOptions* options,
                                                    DartPlantFlutterVmAdapter** out_instance) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "adapter create entered options=%p out=%p", options,
                        out_instance);
    if (out_instance != nullptr) *out_instance = nullptr;
    void* api_dl_data = options == nullptr ? nullptr : options->api_dl_data;
    const uint64_t thread = options == nullptr ? 0 : options->thread;
    const char* snapshot_hash = options == nullptr ? nullptr : options->snapshot_hash;
    if (api_dl_data == nullptr || thread == 0 || snapshot_hash == nullptr ||
        options->struct_size < sizeof(DartPlantFlutterVmAdapterOptions) ||
        options->api_version != DARTPLANT_FLUTTER_VM_ADAPTER_API_VERSION ||
        options->isolate_generation == 0 || out_instance == nullptr) {
        return DARTPLANT_INVALID_ARGUMENT;
    }

    dartplant::VmRuntimeFacts facts{};
    facts.snapshot_hash = snapshot_hash;
    facts.snapshot_features =
        options->snapshot_features == nullptr ? "" : options->snapshot_features;

    if (Dart_InitializeApiDL(api_dl_data) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Dart_InitializeApiDL failed");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "Dart API DL initialized");
    g_is_boolean = reinterpret_cast<DartHandlePredicate>(dlsym(RTLD_DEFAULT, "Dart_IsBoolean"));
    g_is_integer = reinterpret_cast<DartHandlePredicate>(dlsym(RTLD_DEFAULT, "Dart_IsInteger"));
    g_is_double = reinterpret_cast<DartHandlePredicate>(dlsym(RTLD_DEFAULT, "Dart_IsDouble"));
    g_is_string = reinterpret_cast<DartHandlePredicate>(dlsym(RTLD_DEFAULT, "Dart_IsString"));
    const Dart_Isolate isolate = Dart_CurrentIsolate_DL();
    if (isolate == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Dart_CurrentIsolate_DL is null");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const uint64_t current_isolate = CanonicalNativePointer(reinterpret_cast<uint64_t>(isolate));
    __android_log_print(ANDROID_LOG_INFO, kTag, "current isolate=%p canonical=0x%llx", isolate,
                        static_cast<unsigned long long>(current_isolate));
    uint64_t canonical_null = 0;
    if (!SelfTestPersistentApi(&canonical_null)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Dart persistent handle API self-test failed");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "persistent API self-test passed canonical_null=0x%llx",
                        static_cast<unsigned long long>(canonical_null));

    const auto modules = dartplant::EnumerateModules();
    uint32_t engine_module_count = 0;
    for (const auto& module : modules) {
        if (module.name != "libflutter.so") continue;
        ++engine_module_count;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "observed engine incarnation path=%s build_id=%s load_bias=0x%llx",
                            module.path.c_str(), module.build_id.c_str(),
                            static_cast<unsigned long long>(module.load_bias));
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "observed engine module count=%u",
                        static_cast<unsigned>(engine_module_count));

    dartplant::vm_abi::ResolverInput resolver_input{};
    resolver_input.facts = facts;
    resolver_input.thread = thread;
    resolver_input.current_isolate = current_isolate;
    resolver_input.canonical_null = canonical_null;
    resolver_input.modules = &modules;
    resolver_input.engine_anchor = reinterpret_cast<uintptr_t>(Dart_CurrentIsolate_DL);
#if defined(DARTPLANT_FLUTTER_VM_PROFILE_3_4_4)
    resolver_input.only_profile_version = 1;
#endif
    const dartplant::vm_abi::ResolverResult resolution =
        dartplant::vm_abi::ResolveVerifiedBinding(resolver_input);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "ABI resolver snapshot=%s features=%s source_candidates=%zu build_id_gate=disabled",
        snapshot_hash, options->snapshot_features == nullptr ? "" : options->snapshot_features,
        resolution.candidates.size());
    for (size_t index = 0; index < resolution.candidates.size(); ++index) {
        const auto& diagnostic = resolution.candidates[index];
        const auto* candidate = diagnostic.profile;
        const auto& probe = diagnostic.probe;
        const auto& roots = probe.roots;
        __android_log_print(
            probe.passed ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
            "ABI candidate[%zu] profile=%s abi=%s hash_match=%u result=%s stage=%s "
            "heap=0x%llx isolate=0x%llx group=0x%llx null=0x%llx pool=0x%llx "
            "class_table=0x%llx cids=%llu object_store=0x%llx libraries=0x%llx "
            "dart_core=%u register_semantics=%u "
            "enter_code=0x%llx enter=0x%llx exit_code=0x%llx exit=0x%llx module=%s",
            index, candidate->live_vm.name, candidate->abi_id,
            static_cast<unsigned>(diagnostic.snapshot_hash_match), probe.passed ? "pass" : "reject",
            dartplant::vm_abi::CandidateProbeStageName(probe.stage, roots),
            static_cast<unsigned long long>(roots.heap_base),
            static_cast<unsigned long long>(roots.isolate),
            static_cast<unsigned long long>(roots.isolate_group),
            static_cast<unsigned long long>(roots.thread_null),
            static_cast<unsigned long long>(roots.global_object_pool),
            static_cast<unsigned long long>(roots.class_table),
            static_cast<unsigned long long>(roots.num_cids),
            static_cast<unsigned long long>(roots.object_store),
            static_cast<unsigned long long>(roots.libraries),
            static_cast<unsigned>(roots.dart_core_found),
            static_cast<unsigned>(roots.register_semantics_match),
            static_cast<unsigned long long>(probe.enter_code),
            static_cast<unsigned long long>(probe.enter_entry),
            static_cast<unsigned long long>(probe.exit_code),
            static_cast<unsigned long long>(probe.exit_entry),
            probe.code_module == nullptr ? "none" : probe.code_module->path.c_str());
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "ABI resolver summary candidates=%zu passed_rows=%zu distinct_abis=%zu",
                        resolution.candidates.size(), resolution.passed_rows,
                        resolution.distinct_abis);
    __android_log_print(
        resolution.passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
        "DARTPLANT_CI {\"event\":\"core_binding\",\"state\":\"%s\","
        "\"candidate_count\":%zu,\"passing_count\":%zu,\"distinct_abis\":%zu,"
        "\"abi\":\"%s\"}",
        resolution.passed ? "verified" : "rejected", resolution.candidates.size(),
        resolution.passed_rows, resolution.distinct_abis,
        resolution.binding.profile == nullptr ? "none" : resolution.binding.profile->abi_id);
    if (!resolution.passed || resolution.binding.profile == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "VM ABI structural proof rejected: distinct passing ABI count=%zu",
                            resolution.distinct_abis);
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const auto* profile = resolution.binding.profile;
    const auto& selected_probe = resolution.binding.probe;
    if ((resolution.binding.capabilities & dartplant::vm_abi::kCapabilityArtifactLifecycle) == 0 ||
        resolution.binding.artifacts.engines.size() != 1) {
        __android_log_print(
            ANDROID_LOG_ERROR, kTag,
            "VM ABI proof has no unique API-DL engine incarnation anchor=0x%llx matches=%zu",
            static_cast<unsigned long long>(resolver_input.engine_anchor),
            resolution.binding.artifacts.engines.size());
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "verified VM binding abi=%s profile=%s app_path=%s app_build_id=%s "
                        "capabilities=0x%llx engines=%zu engine_anchor=0x%llx "
                        "engine_build_ids_are_diagnostic_only=1",
                        profile->abi_id, profile->live_vm.name,
                        resolution.binding.artifacts.app.path.c_str(),
                        resolution.binding.artifacts.app.build_id.c_str(),
                        static_cast<unsigned long long>(resolution.binding.capabilities),
                        resolution.binding.artifacts.engines.size(),
                        static_cast<unsigned long long>(resolver_input.engine_anchor));

    auto* instance = new (std::nothrow) FlutterVmAdapterImpl;
    if (instance == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "adapter allocation failed status=%d",
                            DARTPLANT_VM_ADAPTER_BUSY);
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    State& state = instance->state;
    state.profile = profile;
    state.artifacts = resolution.binding.artifacts;
    state.capabilities = resolution.binding.capabilities;
    state.verified_capabilities.store(state.capabilities & kEagerRuntimeProofMask,
                                      std::memory_order_release);
    state.failed_capabilities.store(dartplant::vm_abi::kCapabilityNone, std::memory_order_release);
    state.isolate_detached = false;
    state.thread = thread;
    state.enter_safepoint = selected_probe.enter_entry;
    state.exit_safepoint = selected_probe.exit_entry;
    if (!ValidateArtifactLifecycle(state)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "artifact incarnation changed before adapter publication");
        delete instance;
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    MarkCapabilityVerified(state, dartplant::vm_abi::kCapabilityArtifactLifecycle);
    Dart_Handle null_handle = Dart_Null_DL();
    for (auto& root : state.roots) {
        root.handle = Dart_NewPersistentHandle_DL(null_handle);
        if (root.handle == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "persistent root allocation failed at slot=%u",
                                static_cast<unsigned>(&root - state.roots.data()));
            DeleteRoots(state);
            delete instance;
            return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
        }
    }
    state.null_raw = RootRaw(state.roots[0]);
    if (state.null_raw != canonical_null || state.null_raw != selected_probe.roots.thread_null) {
        __android_log_print(
            ANDROID_LOG_ERROR, kTag,
            "verified canonical null changed before root publication expected=0x%llx actual=0x%llx",
            static_cast<unsigned long long>(canonical_null),
            static_cast<unsigned long long>(state.null_raw));
        DeleteRoots(state);
        delete instance;
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    for (const auto& root : state.roots) {
        if (RootRaw(root) != state.null_raw) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "persistent root null mismatch at slot=%u",
                                static_cast<unsigned>(&root - state.roots.data()));
            DeleteRoots(state);
            delete instance;
            return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
        }
    }
    const DartPlantIsolateIdentity identity = {
        .isolate = current_isolate,
        .isolate_group = selected_probe.roots.isolate_group,
        .generation = options->isolate_generation,
    };
    state.identity = identity;
    DartPlantStatus status = dartplant_vm_adapter_create(&kCallbacks, &state, &state.adapter);
    __android_log_print(ANDROID_LOG_INFO, kTag, "core adapter create status=%d", status);
    if (status != DARTPLANT_OK) {
        DeleteRoots(state);
        delete instance;
        return status;
    }
    status = dartplant_vm_adapter_attach_isolate(state.adapter, &identity);
    __android_log_print(ANDROID_LOG_INFO, kTag, "core adapter attach status=%d", status);
    if (status != DARTPLANT_OK) {
        dartplant_vm_adapter_destroy(state.adapter);
        DeleteRoots(state);
        delete instance;
        return status;
    }
    *out_instance = reinterpret_cast<DartPlantFlutterVmAdapter*>(instance);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "source-verified Dart %s V3 adapter initialized abi=%s snapshot=%s thread=0x%llx",
        profile->live_vm.dart_version, profile->abi_id, snapshot_hash,
        static_cast<unsigned long long>(thread));
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "isolate identity backend=%s isolate=0x%llx group=0x%llx generation=%llu",
                        "thread-private-layout", static_cast<unsigned long long>(identity.isolate),
                        static_cast<unsigned long long>(identity.isolate_group),
                        static_cast<unsigned long long>(identity.generation));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_flutter_vm_adapter_destroy(DartPlantFlutterVmAdapter* instance) {
    if (instance == nullptr) return DARTPLANT_OK;
    State& state = reinterpret_cast<FlutterVmAdapterImpl*>(instance)->state;
    if (state.adapter == nullptr) {
        delete reinterpret_cast<FlutterVmAdapterImpl*>(instance);
        return DARTPLANT_OK;
    }
    const DartPlantStatus owner = ValidateCurrentOwner(state);
    if (owner != DARTPLANT_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "refusing adapter destroy outside its Dart isolate/group");
        return owner;
    }
    if (!state.isolate_detached) {
        DartPlantStatus detach_status =
            dartplant_vm_adapter_detach_isolate(state.adapter, &state.identity);
        if (detach_status != DARTPLANT_OK) return detach_status;
        state.isolate_detached = true;
    }
    DartPlantStatus status = DARTPLANT_OK;
    status = dartplant_vm_adapter_destroy(state.adapter);
    if (status == DARTPLANT_OK) {
        DeleteRoots(state);
        state.adapter = nullptr;
        delete reinterpret_cast<FlutterVmAdapterImpl*>(instance);
    }
    return status;
}

DartPlantVmAdapter* dartplant_flutter_vm_adapter_get(DartPlantFlutterVmAdapter* instance) {
    return instance == nullptr ? nullptr
                               : reinterpret_cast<FlutterVmAdapterImpl*>(instance)->state.adapter;
}

uint64_t dartplant_flutter_vm_adapter_capabilities(const DartPlantFlutterVmAdapter* instance) {
    return instance == nullptr
               ? 0
               : reinterpret_cast<const FlutterVmAdapterImpl*>(instance)->state.capabilities;
}

uint64_t dartplant_flutter_vm_adapter_verified_capabilities(
    const DartPlantFlutterVmAdapter* instance) {
    return instance == nullptr ? 0
                               : reinterpret_cast<const FlutterVmAdapterImpl*>(instance)
                                     ->state.verified_capabilities.load(std::memory_order_acquire);
}

uint64_t dartplant_flutter_vm_adapter_failed_capabilities(
    const DartPlantFlutterVmAdapter* instance) {
    return instance == nullptr ? 0
                               : reinterpret_cast<const FlutterVmAdapterImpl*>(instance)
                                     ->state.failed_capabilities.load(std::memory_order_acquire);
}

DartPlantFlutterVmProofState dartplant_flutter_vm_adapter_capability_state(
    const DartPlantFlutterVmAdapter* instance, DartPlantFlutterVmCapability capability) {
    if (instance == nullptr) return DARTPLANT_FLUTTER_VM_PROOF_UNSUPPORTED;
    const uint64_t mask = static_cast<uint64_t>(capability);
    if (mask == 0 || (mask & (mask - 1)) != 0) {
        return DARTPLANT_FLUTTER_VM_PROOF_UNSUPPORTED;
    }
    const State& state = reinterpret_cast<const FlutterVmAdapterImpl*>(instance)->state;
    if (!HasCapability(state, mask)) return DARTPLANT_FLUTTER_VM_PROOF_UNSUPPORTED;
    if (HasFailedCapability(state, mask)) {
        return DARTPLANT_FLUTTER_VM_PROOF_FAILED_FOR_INCARNATION;
    }
    return HasVerifiedCapability(state, mask) ? DARTPLANT_FLUTTER_VM_PROOF_VERIFIED
                                              : DARTPLANT_FLUTTER_VM_PROOF_UNVERIFIED;
}

uint64_t dartplant_flutter_vm_adapter_artifact_generation(
    const DartPlantFlutterVmAdapter* instance) {
    return instance == nullptr ? 0
                               : reinterpret_cast<const FlutterVmAdapterImpl*>(instance)
                                     ->state.artifact_generation.load(std::memory_order_acquire);
}

void dartplant_flutter_vm_adapter_invalidate_artifacts(DartPlantFlutterVmAdapter* instance) {
    if (instance == nullptr) return;
    State& state = reinterpret_cast<FlutterVmAdapterImpl*>(instance)->state;
    dartplant::VmAdapterCloseAdmission(state.adapter);
    std::lock_guard artifact_lock(state.artifact_mutex);
    state.artifact_quiescing.store(true, std::memory_order_release);
    const uint64_t artifact_generation = InvalidateArtifactBindingLocked(state);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "artifact binding invalidated abi=%s isolate_generation=%llu "
                        "artifact_generation=%llu",
                        state.profile == nullptr ? "none" : state.profile->abi_id,
                        static_cast<unsigned long long>(state.identity.generation),
                        static_cast<unsigned long long>(artifact_generation));
}

DartPlantStatus dartplant_flutter_vm_adapter_quiesce_artifacts(
    DartPlantFlutterVmAdapter* instance) {
    if (instance == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    State& state = reinterpret_cast<FlutterVmAdapterImpl*>(instance)->state;
    if (state.adapter == nullptr) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    {
        std::lock_guard artifact_lock(state.artifact_mutex);
        state.artifact_quiescing.store(true, std::memory_order_release);
    }
    const DartPlantStatus hook_status = dartplant::QuiesceVmAdapterHooks(state.adapter);
    if (hook_status != DARTPLANT_OK) return hook_status;
    return dartplant::VmAdapterCheckQuiescent(state.adapter);
}

DartPlantStatus dartplant_flutter_vm_adapter_retire_artifacts(DartPlantFlutterVmAdapter* instance) {
    if (instance == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    State& state = reinterpret_cast<FlutterVmAdapterImpl*>(instance)->state;
    const DartPlantStatus quiesce = dartplant_flutter_vm_adapter_quiesce_artifacts(instance);
    if (quiesce != DARTPLANT_OK) return quiesce;

    std::lock_guard artifact_lock(state.artifact_mutex);
    if (!state.artifact_valid.load(std::memory_order_acquire)) return DARTPLANT_OK;
    const uint64_t artifact_generation = InvalidateArtifactBindingLocked(state);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "artifact binding retired abi=%s isolate_generation=%llu "
                        "artifact_generation=%llu",
                        state.profile == nullptr ? "none" : state.profile->abi_id,
                        static_cast<unsigned long long>(state.identity.generation),
                        static_cast<unsigned long long>(artifact_generation));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_flutter_vm_adapter_revalidate_artifacts(
    DartPlantFlutterVmAdapter* instance) {
    if (instance == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    State& state = reinterpret_cast<FlutterVmAdapterImpl*>(instance)->state;
    std::lock_guard artifact_lock(state.artifact_mutex);
    if (state.profile == nullptr || state.adapter == nullptr) {
        state.artifact_valid.store(false, std::memory_order_release);
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (state.artifact_quiescing.load(std::memory_order_acquire) &&
        state.artifact_valid.load(std::memory_order_acquire)) {
        dartplant::SetLastError("artifact retirement is still quiescing active work");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    if (!ValidateArtifactLifecycle(state)) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "artifact binding revalidation rejected abi=%s isolate_generation=%llu "
                            "artifact_generation=%llu; new resolver required",
                            state.profile->abi_id,
                            static_cast<unsigned long long>(state.identity.generation),
                            static_cast<unsigned long long>(
                                state.artifact_generation.load(std::memory_order_acquire)));
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    MarkCapabilityVerified(state, dartplant::vm_abi::kCapabilityArtifactLifecycle |
                                      dartplant::vm_abi::kCapabilitySafepointStubs);
    state.artifact_quiescing.store(false, std::memory_order_release);
    dartplant::VmAdapterOpenAdmission(state.adapter);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "artifact binding revalidated abi=%s isolate_generation=%llu "
        "artifact_generation=%llu",
        state.profile->abi_id, static_cast<unsigned long long>(state.identity.generation),
        static_cast<unsigned long long>(state.artifact_generation.load(std::memory_order_acquire)));
    return DARTPLANT_OK;
}

const DartPlantFlutterVmDescriptor* DescriptorAt(uint32_t index) {
    static const auto descriptors = [] {
        std::array<DartPlantFlutterVmDescriptor, std::size(kDescriptorMetadata)> result{};
        for (size_t cursor = 0; cursor < result.size(); ++cursor) {
            const auto& metadata = kDescriptorMetadata[cursor];
            const auto* profile = dartplant::FindRuntimeProfileByVersion(metadata.profile_version);
            if (profile == nullptr) continue;
            result[cursor] = {
                .struct_size = sizeof(DartPlantFlutterVmDescriptor),
                .descriptor_version = 1,
                .vm_adapter_version = 3,
                .descriptor_id = metadata.descriptor_id,
                .dart_version = profile->live_vm.dart_version,
                .flutter_version = metadata.flutter_version,
                .snapshot_hash = profile->live_vm.snapshot_hash,
                .flutter_module_name = "libflutter.so",
                // Deprecated compatibility field. Engine Build IDs are
                // observed as artifact-incarnation diagnostics and never
                // select a private VM ABI.
                .flutter_build_id = "",
                .pointer_size = profile->machine.pointer_size,
                .compressed_pointers =
                    profile->machine.compressed_pointers ? uint8_t{1} : uint8_t{0},
                .product_mode = profile->machine.product ? uint8_t{1} : uint8_t{0},
                .reserved = {0, 0},
            };
        }
        return result;
    }();
    return index < descriptors.size() ? &descriptors[index] : nullptr;
}

uint32_t dartplant_flutter_vm_descriptor_count(void) { return std::size(kDescriptorMetadata); }

const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_descriptor_at(uint32_t index) {
    return DescriptorAt(index);
}

const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_adapter_descriptor(
    const DartPlantFlutterVmAdapter* instance) {
    if (instance == nullptr) return nullptr;
    const auto* impl = reinterpret_cast<const FlutterVmAdapterImpl*>(instance);
    if (impl->state.profile == nullptr) return nullptr;
    for (uint32_t index = 0; index < std::size(kDescriptorMetadata); ++index) {
        if (kDescriptorMetadata[index].profile_version ==
            impl->state.profile->live_vm.profile_version) {
            return DescriptorAt(index);
        }
    }
    return nullptr;
}
