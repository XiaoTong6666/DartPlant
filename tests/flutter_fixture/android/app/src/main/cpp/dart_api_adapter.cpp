#include "dart_api_adapter.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include <array>
#include <atomic>
#include <cstring>

#include "dart_api_dl.h"

namespace {

struct RealHandle {
    Dart_PersistentHandle strong = nullptr;
    Dart_WeakPersistentHandle weak = nullptr;
    DartPlantObjectStrength strength = DARTPLANT_OBJECT_STRONG;
    std::atomic<bool> alive = true;
    uint32_t root_slot = UINT32_MAX;
};

constexpr uint32_t kRootSlotCount = 384;
constexpr uint32_t kLeaseCount = 4;
constexpr uint32_t kRootsPerLease = 96;
constexpr char kExactSnapshotHash[] = "d20a1be77c3d3c41b2a5accaee1ce549";
constexpr std::array<uint8_t, 20> kExactFlutterBuildId = {
    0xb4, 0xba, 0x48, 0xb1, 0x6f, 0x17, 0x60, 0x76, 0x3d, 0x44,
    0x4d, 0x35, 0xd5, 0xd0, 0x10, 0x6b, 0xa5, 0x54, 0xfa, 0xbd,
};

struct RootSlot {
    Dart_PersistentHandle handle = nullptr;
    std::atomic_bool used{false};
};

struct RootLease {
    std::atomic_bool used{false};
    uint32_t count = 0;
    std::array<uint16_t, kRootsPerLease> slots{};
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
};

State g_state;
constexpr char kTag[] = "DartPlantFixture";
using DartHandlePredicate = bool (*)(Dart_Handle);
DartHandlePredicate g_is_boolean = nullptr;
DartHandlePredicate g_is_integer = nullptr;
DartHandlePredicate g_is_double = nullptr;
DartHandlePredicate g_is_string = nullptr;

extern "C" void dartplant_fixture_call_vm_safepoint_stub(uint64_t thread, uint64_t entry);

constexpr uintptr_t kThreadEnterSafepointStub = 0x1d0;
constexpr uintptr_t kThreadExitSafepointStub = 0x1d8;
constexpr uintptr_t kThreadIsolate = 0x6f0;
constexpr uintptr_t kThreadIsolateGroup = 0x6f8;
constexpr uintptr_t kThreadTopExitFrame = 0x710;
constexpr uintptr_t kThreadVmTag = 0x730;
constexpr uintptr_t kThreadExecutionState = 0x770;
constexpr uintptr_t kThreadExitThroughFfi = 0x780;
constexpr uintptr_t kCodeEntryPoint = 0x8;
constexpr uint64_t kVmTagDart = 8;
constexpr uint64_t kExecutionGenerated = 1;
constexpr uint64_t kExecutionNative = 2;
constexpr uint64_t kExitThroughFfi = 1;

uint64_t& ThreadWord(uintptr_t offset) {
    return *reinterpret_cast<uint64_t*>(g_state.thread + offset);
}

uint64_t RootRaw(const RootSlot& slot) { return *reinterpret_cast<const uint64_t*>(slot.handle); }

void SetRootRaw(RootSlot& slot, uint64_t raw) { *reinterpret_cast<uint64_t*>(slot.handle) = raw; }

bool AcquireRoot(uint32_t* out_index) {
    if (out_index == nullptr) return false;
    for (uint32_t index = 0; index < g_state.roots.size(); ++index) {
        bool expected = false;
        if (g_state.roots[index].used.compare_exchange_strong(expected, true,
                                                              std::memory_order_acq_rel)) {
            *out_index = index;
            return true;
        }
    }
    return false;
}

void ReleaseRoot(uint32_t index) {
    if (index >= g_state.roots.size()) return;
    SetRootRaw(g_state.roots[index], g_state.null_raw);
    g_state.roots[index].used.store(false, std::memory_order_release);
}

uint64_t ResolveStubEntry(uintptr_t offset) {
    const uint64_t tagged_code = ThreadWord(offset);
    if ((tagged_code & 1) == 0 || tagged_code <= 1) return 0;
    const uint64_t entry = *reinterpret_cast<const uint64_t*>(tagged_code - 1 + kCodeEntryPoint);
    Dl_info info{};
    const bool valid = entry != 0 && dladdr(reinterpret_cast<void*>(entry), &info) != 0 &&
                       info.dli_fname != nullptr &&
                       std::strstr(info.dli_fname, "libapp.so") != nullptr;
    return valid ? entry : 0;
}

uintptr_t AlignNote(uintptr_t value) { return (value + 3) & ~uintptr_t{3}; }

int MatchFlutterBuildId(dl_phdr_info* info, size_t, void* data) {
    auto* matched = static_cast<bool*>(data);
    if (info == nullptr || matched == nullptr || info->dlpi_name == nullptr ||
        std::strstr(info->dlpi_name, "libflutter.so") == nullptr) {
        return 0;
    }
    for (uint16_t index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) & phdr = info->dlpi_phdr[index];
        if (phdr.p_type != PT_NOTE) continue;
        uintptr_t cursor = info->dlpi_addr + phdr.p_vaddr;
        const uintptr_t end = cursor + phdr.p_memsz;
        while (cursor <= end && end - cursor >= sizeof(ElfW(Nhdr))) {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(cursor);
            cursor += sizeof(*note);
            const uintptr_t name = cursor;
            cursor = AlignNote(cursor + note->n_namesz);
            const uintptr_t description = cursor;
            cursor = AlignNote(cursor + note->n_descsz);
            if (cursor > end) break;
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
                note->n_descsz == kExactFlutterBuildId.size() &&
                std::memcmp(reinterpret_cast<const void*>(name), "GNU", 4) == 0 &&
                std::memcmp(reinterpret_cast<const void*>(description), kExactFlutterBuildId.data(),
                            kExactFlutterBuildId.size()) == 0) {
                *matched = true;
                return 1;
            }
        }
    }
    return 1;
}

bool ExactFlutterBuildMatches() {
    bool matched = false;
    dl_iterate_phdr(MatchFlutterBuildId, &matched);
    return matched;
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

DartPlantStatus RetainObject(void*, const DartPlantIsolateIdentity*, uint64_t raw,
                             DartPlantObjectStrength strength, void** out_backend) {
    if (out_backend == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    uint32_t slot_index = UINT32_MAX;
    if (!AcquireRoot(&slot_index)) return DARTPLANT_VM_ADAPTER_BUSY;
    RootSlot& slot = g_state.roots[slot_index];
    SetRootRaw(slot, raw);
    Dart_Handle local = Dart_HandleFromPersistent_DL(slot.handle);
    if (local == nullptr) {
        ReleaseRoot(slot_index);
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    auto* handle = new RealHandle;
    handle->strength = strength;
    if (strength == DARTPLANT_OBJECT_WEAK) {
        handle->weak = Dart_NewWeakPersistentHandle_DL(local, handle, 0, WeakFinalizer);
        ReleaseRoot(slot_index);
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
        ReleaseRoot(handle->root_slot);
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
        *out_raw = RootRaw(g_state.roots[handle->root_slot]);
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

bool SelfTestPersistentApi() {
    Dart_EnterScope_DL();
    Dart_Handle null_handle = Dart_Null_DL();
    Dart_PersistentHandle strong = Dart_NewPersistentHandle_DL(null_handle);
    Dart_WeakPersistentHandle weak =
        Dart_NewWeakPersistentHandle_DL(null_handle, nullptr, 0, WeakFinalizer);
    const bool valid = strong != nullptr && weak != nullptr;
    if (weak != nullptr) Dart_DeleteWeakPersistentHandle_DL(weak);
    if (strong != nullptr) Dart_DeletePersistentHandle_DL(strong);
    Dart_ExitScope_DL();
    return valid;
}

DartPlantStatus PinGeneratedRoots(void*, const DartPlantIsolateIdentity* identity,
                                  const uint64_t* raw_values, uint32_t value_count,
                                  void** out_root_lease) {
    if (identity == nullptr || raw_values == nullptr || out_root_lease == nullptr ||
        value_count == 0 || value_count > kRootsPerLease ||
        identity->isolate != g_state.identity.isolate ||
        identity->isolate_group != g_state.identity.isolate_group) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    RootLease* lease = nullptr;
    for (auto& candidate : g_state.leases) {
        bool expected = false;
        if (candidate.used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            lease = &candidate;
            break;
        }
    }
    if (lease == nullptr) return DARTPLANT_VM_ADAPTER_BUSY;
    lease->count = 0;
    for (uint32_t index = 0; index < value_count; ++index) {
        uint32_t root = UINT32_MAX;
        if (!AcquireRoot(&root)) {
            for (uint32_t cursor = 0; cursor < lease->count; ++cursor) {
                ReleaseRoot(lease->slots[cursor]);
            }
            lease->used.store(false, std::memory_order_release);
            return DARTPLANT_VM_ADAPTER_BUSY;
        }
        lease->slots[index] = static_cast<uint16_t>(root);
        SetRootRaw(g_state.roots[root], raw_values[index]);
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
        index >= lease->count) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_raw = RootRaw(g_state.roots[lease->slots[index]]);
    return DARTPLANT_OK;
}

DartPlantStatus GeneratedRootSet(void*, const DartPlantIsolateIdentity*, void* root_lease,
                                 uint32_t index, uint64_t raw) {
    auto* lease = static_cast<RootLease*>(root_lease);
    if (lease == nullptr || !lease->used.load(std::memory_order_acquire) || index >= lease->count) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    SetRootRaw(g_state.roots[lease->slots[index]], raw);
    return DARTPLANT_OK;
}

DartPlantStatus UnpinGeneratedRoots(void*, const DartPlantIsolateIdentity*, void* root_lease,
                                    uint64_t* out_raw_values, uint32_t value_count) {
    auto* lease = static_cast<RootLease*>(root_lease);
    if (lease == nullptr || out_raw_values == nullptr ||
        !lease->used.load(std::memory_order_acquire) || value_count != lease->count) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < lease->count; ++index) {
        out_raw_values[index] = RootRaw(g_state.roots[lease->slots[index]]);
        ReleaseRoot(lease->slots[index]);
    }
    lease->count = 0;
    lease->used.store(false, std::memory_order_release);
    return DARTPLANT_OK;
}

DartPlantStatus EnterGeneratedToNative(void*, const DartPlantIsolateIdentity* identity,
                                       const DartPlantGeneratedTransitionFrame* frame, void*) {
    if (identity == nullptr || frame == nullptr || frame->thread != g_state.thread ||
        identity->isolate != g_state.identity.isolate ||
        (frame->flags & DARTPLANT_GENERATED_TRANSITION_SYNTHETIC_EXIT_FRAME) == 0 ||
        ThreadWord(kThreadExecutionState) != kExecutionGenerated ||
        ThreadWord(kThreadTopExitFrame) != 0 || ThreadWord(kThreadExitThroughFfi) != 0 ||
        ThreadWord(kThreadVmTag) != kVmTagDart) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    ThreadWord(kThreadTopExitFrame) = frame->exit_frame;
    ThreadWord(kThreadExitThroughFfi) = kExitThroughFfi;
    ThreadWord(kThreadVmTag) = reinterpret_cast<uint64_t>(&EnterGeneratedToNative);
    ThreadWord(kThreadExecutionState) = kExecutionNative;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    dartplant_fixture_call_vm_safepoint_stub(g_state.thread, g_state.enter_safepoint);
    return DARTPLANT_OK;
}

DartPlantStatus LeaveNativeToGenerated(void*, const DartPlantIsolateIdentity* identity,
                                       const DartPlantGeneratedTransitionFrame* frame, void*) {
    if (identity == nullptr || frame == nullptr || frame->thread != g_state.thread ||
        ThreadWord(kThreadExecutionState) != kExecutionNative ||
        ThreadWord(kThreadTopExitFrame) != frame->exit_frame ||
        ThreadWord(kThreadExitThroughFfi) != kExitThroughFfi) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    dartplant_fixture_call_vm_safepoint_stub(g_state.thread, g_state.exit_safepoint);
    ThreadWord(kThreadVmTag) = kVmTagDart;
    ThreadWord(kThreadExecutionState) = kExecutionGenerated;
    ThreadWord(kThreadTopExitFrame) = 0;
    ThreadWord(kThreadExitThroughFfi) = 0;
    std::atomic_thread_fence(std::memory_order_seq_cst);
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
};

}  // namespace

DartPlantStatus dartplant_fixture_create_dart_api_adapter(void* api_dl_data, uint64_t thread,
                                                          const char* snapshot_hash,
                                                          DartPlantVmAdapter** out_adapter) {
    if (api_dl_data == nullptr || thread == 0 || snapshot_hash == nullptr ||
        out_adapter == nullptr || std::strcmp(snapshot_hash, kExactSnapshotHash) != 0) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (!ExactFlutterBuildMatches()) return DARTPLANT_PROFILE_MISMATCH;
    if (Dart_InitializeApiDL(api_dl_data) != 0) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    g_is_boolean = reinterpret_cast<DartHandlePredicate>(dlsym(RTLD_DEFAULT, "Dart_IsBoolean"));
    g_is_integer = reinterpret_cast<DartHandlePredicate>(dlsym(RTLD_DEFAULT, "Dart_IsInteger"));
    g_is_double = reinterpret_cast<DartHandlePredicate>(dlsym(RTLD_DEFAULT, "Dart_IsDouble"));
    g_is_string = reinterpret_cast<DartHandlePredicate>(dlsym(RTLD_DEFAULT, "Dart_IsString"));
    const Dart_Isolate isolate = Dart_CurrentIsolate_DL();
    if (isolate == nullptr) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    if (!SelfTestPersistentApi()) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Dart persistent handle API self-test failed");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    g_state.thread = thread;
    const uint64_t thread_isolate = ThreadWord(kThreadIsolate);
    const uint64_t group = ThreadWord(kThreadIsolateGroup);
    if (thread_isolate != reinterpret_cast<uint64_t>(isolate) || group == 0) {
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    g_state.enter_safepoint = ResolveStubEntry(kThreadEnterSafepointStub);
    g_state.exit_safepoint = ResolveStubEntry(kThreadExitSafepointStub);
    if (g_state.enter_safepoint == 0 || g_state.exit_safepoint == 0) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    Dart_Handle null_handle = Dart_Null_DL();
    for (auto& root : g_state.roots) {
        root.handle = Dart_NewPersistentHandle_DL(null_handle);
        if (root.handle == nullptr) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    g_state.null_raw = RootRaw(g_state.roots[0]);
    for (const auto& root : g_state.roots) {
        if (RootRaw(root) != g_state.null_raw) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const DartPlantIsolateIdentity identity = {
        .isolate = reinterpret_cast<uint64_t>(isolate),
        .isolate_group = group,
        .generation = 1,
    };
    g_state.identity = identity;
    DartPlantStatus status = dartplant_vm_adapter_create(&kCallbacks, &g_state, &g_state.adapter);
    if (status != DARTPLANT_OK) return status;
    status = dartplant_vm_adapter_attach_isolate(g_state.adapter, &identity);
    if (status != DARTPLANT_OK) {
        dartplant_vm_adapter_destroy(g_state.adapter);
        g_state.adapter = nullptr;
        return status;
    }
    *out_adapter = g_state.adapter;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "exact Dart 3.4.4 V3 adapter initialized snapshot=%s thread=0x%llx",
                        snapshot_hash, static_cast<unsigned long long>(thread));
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "isolate identity backend=%s isolate=0x%llx group=0x%llx generation=%llu",
                        "thread-private-layout", static_cast<unsigned long long>(identity.isolate),
                        static_cast<unsigned long long>(identity.isolate_group),
                        static_cast<unsigned long long>(identity.generation));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_fixture_destroy_dart_api_adapter() {
    if (g_state.adapter == nullptr) return DARTPLANT_OK;
    DartPlantStatus status =
        dartplant_vm_adapter_detach_isolate(g_state.adapter, &g_state.identity);
    if (status != DARTPLANT_OK) return status;
    status = dartplant_vm_adapter_destroy(g_state.adapter);
    if (status == DARTPLANT_OK) {
        for (auto& root : g_state.roots) {
            if (root.handle != nullptr) Dart_DeletePersistentHandle_DL(root.handle);
            root.handle = nullptr;
            root.used.store(false, std::memory_order_release);
        }
        g_state.adapter = nullptr;
        dartplant_fixture_shutdown_raw_bridge();
    }
    return status;
}

DartPlantVmAdapter* dartplant_fixture_dart_api_adapter() { return g_state.adapter; }

bool dartplant_fixture_dart_api_adapter_ready_for_hooks() {
    return g_state.adapter != nullptr && g_state.enter_safepoint != 0 &&
           g_state.exit_safepoint != 0;
}

void dartplant_fixture_set_raw_handle_bridge(DartPlantRawToHandle raw_to_handle,
                                             DartPlantHandleToRaw handle_to_raw,
                                             DartPlantCurrentIdentity current_identity) {
    dartplant_fixture_set_engine_raw_handle_shim(raw_to_handle, handle_to_raw, current_identity);
}
