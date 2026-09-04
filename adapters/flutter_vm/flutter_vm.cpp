#include "dartplant/adapters/flutter_vm.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "dart_api_dl.h"

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
    const struct AdapterProfile* profile = nullptr;
};

struct AdapterLayout {
    uintptr_t heap_base;
    uintptr_t enter_safepoint_stub;
    uintptr_t exit_safepoint_stub;
    uintptr_t isolate;
    uintptr_t isolate_group;
    uintptr_t top_exit_frame;
    uintptr_t vm_tag;
    uintptr_t execution_state;
    uintptr_t exit_through_ffi;
    uintptr_t active_exception;
    uintptr_t active_stacktrace;
};

struct TypeArgumentsLayout {
    uint32_t cid;
    uintptr_t length;
    uintptr_t types;
    uint32_t compressed_word_size;
    uint8_t heap_object_tag;
    uint8_t smi_tag_mask;
    uint8_t smi_tag_shift;
    uint8_t class_id_tag_shift;
    uint8_t class_id_tag_bits;
};

struct AdapterProfile {
    const char* dart_version;
    const char* flutter_version;
    const char* snapshot_hash;
    const char* flutter_build_id;
    const char* descriptor_id;
    const char* module_name;
    AdapterLayout layout;
    TypeArgumentsLayout type_arguments;
};

struct FlutterVmAdapterImpl {
    State state;
};

constexpr AdapterProfile kProfiles[] = {
    {"3.4.4",
     "3.22.3",
     "d20a1be77c3d3c41b2a5accaee1ce549",
     "b4ba48b16f1760763d444d35d5d0106ba554fabd",
     "flutter-3.22.3-dart-3.4.4-android-arm64-product",
     "libapp.so",
     {0x48, 0x1d0, 0x1d8, 0x6f0, 0x6f8, 0x710, 0x730, 0x770, 0x780, 0x748, 0x750},
     {46, 0x0c, 0x18, 4, 1, 1, 1, 12, 20}},
#if !defined(DARTPLANT_FLUTTER_VM_PROFILE_3_4_4)
    {"3.5.0",
     "3.24.0",
     "80a49c7111088100a233b2ae788e1f48",
     "d9a7f562e5595e9913262ba4b5026a0df242ccfd",
     "flutter-3.24.0-dart-3.5.0-android-arm64-product",
     "libapp.so",
     {0x48, 0x1d8, 0x1e0, 0x708, 0x710, 0x728, 0x750, 0x790, 0x7a0, 0x768, 0x770},
     {46, 0x0c, 0x18, 4, 1, 1, 1, 12, 20}},
    {"3.12.1",
     "3.44.1",
     "ace654289f5abc240509fc941453ebc5",
     "ca4618220c6646c3546f020f587fef1c75e3c505",
     "flutter-3.44.1-dart-3.12.1-android-arm64-product",
     "libapp.so",
     {0x58, 0x1e8, 0x1f0, 0x680, 0x688, 0x6a0, 0x6c8, 0x6f8, 0x708, 0x6d0, 0x6d8},
     {47, 0x0c, 0x18, 4, 1, 1, 1, 12, 20}},
#endif
};

#if defined(DARTPLANT_FLUTTER_VM_PROFILE_3_4_4)
static_assert(std::size(kProfiles) == 1,
              "the version-specific Flutter VM adapter must expose only Dart 3.4.4");
#endif

constexpr char kTag[] = "DartPlantFlutterVm";
using DartHandlePredicate = bool (*)(Dart_Handle);
DartHandlePredicate g_is_boolean = nullptr;
DartHandlePredicate g_is_integer = nullptr;
DartHandlePredicate g_is_double = nullptr;
DartHandlePredicate g_is_string = nullptr;

extern "C" void dartplant_flutter_vm_call_safepoint_stub(uint64_t thread, uint64_t entry);

constexpr uintptr_t kCodeEntryPoint = 0x8;
constexpr uint64_t kVmTagDart = 8;
constexpr uint64_t kExecutionGenerated = 1;
constexpr uint64_t kExecutionNative = 2;
constexpr uint64_t kExitThroughFfi = 1;
constexpr uint64_t kExitThroughRuntimeCall = 2;

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

uint64_t ResolveStubEntry(State& state, uintptr_t offset) {
    const uint64_t tagged_code = ThreadWord(state, offset);
    if ((tagged_code & 1) == 0 || tagged_code <= 1) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "safepoint slot invalid offset=0x%zx raw=0x%llx", offset,
                            static_cast<unsigned long long>(tagged_code));
        return 0;
    }
    const uint64_t entry = *reinterpret_cast<const uint64_t*>(tagged_code - 1 + kCodeEntryPoint);
    Dl_info info{};
    const bool valid = entry != 0 && dladdr(reinterpret_cast<void*>(entry), &info) != 0 &&
                       info.dli_fname != nullptr &&
                       std::strstr(info.dli_fname, "libapp.so") != nullptr;
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "safepoint offset=0x%zx code=0x%llx entry=0x%llx module=%s valid=%u", offset,
        static_cast<unsigned long long>(tagged_code), static_cast<unsigned long long>(entry),
        info.dli_fname == nullptr ? "none" : info.dli_fname, static_cast<unsigned>(valid));
    return valid ? entry : 0;
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

uintptr_t AlignNote(uintptr_t value) { return (value + 3) & ~uintptr_t{3}; }

int MatchFlutterBuildId(dl_phdr_info* info, size_t, void* data) {
    auto* request = static_cast<std::pair<const char*, bool>*>(data);
    auto* matched = request == nullptr ? nullptr : &request->second;
    if (info == nullptr || matched == nullptr || request == nullptr || info->dlpi_name == nullptr ||
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
                std::memcmp(reinterpret_cast<const void*>(name), "GNU", 4) == 0 &&
                request->first != nullptr && std::strlen(request->first) == note->n_descsz * 2) {
                bool equal = true;
                for (uint32_t byte = 0; byte < note->n_descsz; ++byte) {
                    const int high = HexValue(request->first[byte * 2]);
                    const int low = HexValue(request->first[byte * 2 + 1]);
                    if (high < 0 || low < 0 ||
                        static_cast<uint8_t>((high << 4) | low) !=
                            reinterpret_cast<const uint8_t*>(description)[byte]) {
                        equal = false;
                        break;
                    }
                }
                if (equal) {
                    *matched = true;
                    return 1;
                }
            }
        }
    }
    return 1;
}

bool ExactFlutterBuildMatches(const char* build_id) {
    std::pair<const char*, bool> request{build_id, false};
    dl_iterate_phdr(MatchFlutterBuildId, &request);
    __android_log_print(ANDROID_LOG_INFO, kTag, "build fingerprint %s match=%u", build_id,
                        static_cast<unsigned>(request.second));
    return request.second;
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
    const uint64_t execution_state = ThreadWord(*state, state->profile->layout.execution_state);
    const uint64_t top_exit_frame = ThreadWord(*state, state->profile->layout.top_exit_frame);
    uint64_t& exit_through_ffi = ThreadWord(*state, state->profile->layout.exit_through_ffi);
    const uint64_t vm_tag = ThreadWord(*state, state->profile->layout.vm_tag);
    if (execution_state != kExecutionGenerated || top_exit_frame != 0 || vm_tag != kVmTagDart) {
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
    if (exit_through_ffi == kExitThroughRuntimeCall) {
        exit_through_ffi = 0;
    } else if (exit_through_ffi != 0) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    ThreadWord(*state, state->profile->layout.top_exit_frame) = frame->exit_frame;
    exit_through_ffi = kExitThroughFfi;
    ThreadWord(*state, state->profile->layout.vm_tag) =
        reinterpret_cast<uint64_t>(&EnterGeneratedToNative);
    ThreadWord(*state, state->profile->layout.execution_state) = kExecutionNative;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    dartplant_flutter_vm_call_safepoint_stub(state->thread, state->enter_safepoint);
    return DARTPLANT_OK;
}

DartPlantStatus LeaveNativeToGenerated(void* user_data, const DartPlantIsolateIdentity* identity,
                                       const DartPlantGeneratedTransitionFrame* frame, void*) {
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr || identity == nullptr || frame == nullptr ||
        frame->thread != state->thread ||
        ThreadWord(*state, state->profile->layout.execution_state) != kExecutionNative ||
        ThreadWord(*state, state->profile->layout.top_exit_frame) != frame->exit_frame ||
        ThreadWord(*state, state->profile->layout.exit_through_ffi) != kExitThroughFfi) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    dartplant_flutter_vm_call_safepoint_stub(state->thread, state->exit_safepoint);
    ThreadWord(*state, state->profile->layout.vm_tag) = kVmTagDart;
    ThreadWord(*state, state->profile->layout.execution_state) = kExecutionGenerated;
    ThreadWord(*state, state->profile->layout.top_exit_frame) = 0;
    ThreadWord(*state, state->profile->layout.exit_through_ffi) = 0;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return DARTPLANT_OK;
}

DartPlantStatus ReadActiveException(void* user_data, const DartPlantIsolateIdentity* identity,
                                    uint64_t* out_raw) {
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr || identity == nullptr || out_raw == nullptr ||
        identity->isolate != state->identity.isolate || state->profile == nullptr) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_raw = ThreadWord(*state, state->profile->layout.active_exception);
    return DARTPLANT_OK;
}

DartPlantStatus ReadActiveStacktrace(void* user_data, const DartPlantIsolateIdentity* identity,
                                     uint64_t* out_raw) {
    auto* state = static_cast<State*>(user_data);
    if (state == nullptr || identity == nullptr || out_raw == nullptr ||
        identity->isolate != state->identity.isolate || state->profile == nullptr) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_raw = ThreadWord(*state, state->profile->layout.active_stacktrace);
    return DARTPLANT_OK;
}

DartPlantStatus ReadTypeArgumentsElement(void* user_data, const DartPlantIsolateIdentity* identity,
                                         uint64_t type_arguments_raw, uint32_t index,
                                         uint64_t* out_raw) {
    auto* state = static_cast<State*>(user_data);
    const auto* type_arguments =
        state == nullptr || state->profile == nullptr ? nullptr : &state->profile->type_arguments;
    if (state == nullptr || identity == nullptr || out_raw == nullptr ||
        type_arguments == nullptr || identity->isolate != state->identity.isolate ||
        identity->isolate_group != state->identity.isolate_group ||
        identity->generation != state->identity.generation ||
        (type_arguments_raw & type_arguments->smi_tag_mask) != type_arguments->heap_object_tag) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    // This callback is invoked by dartplant_core before Generated->Native and
    // before any Dart API scope/safepoint. The mutator therefore cannot move
    // this object while these raw compressed fields are inspected. Every
    // returned element is immediately copied into the generated-root lease;
    // user callbacks never retain or dereference this object address.
    const uint64_t heap_base = ThreadWord(*state, state->profile->layout.heap_base);
    const uintptr_t tagged_object = static_cast<uintptr_t>(type_arguments_raw);
    // Generated Dart registers/stack slots contain a full tagged ObjectPtr even
    // in compressed-pointer builds. Only fields inside heap objects are stored
    // as 32-bit compressed pointers. Constrain the full pointer to this heap's
    // 4-GiB compression window before reading its header.
    if (heap_base == 0 || tagged_object < heap_base + 1 || tagged_object - heap_base > UINT32_MAX) {
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    const uintptr_t object = tagged_object - 1;

    uint64_t tags = 0;
    if (!ReadSelf(object, &tags)) return DARTPLANT_OBJECT_HANDLE_INVALID;
    if (type_arguments->class_id_tag_bits == 0 || type_arguments->class_id_tag_bits >= 64) {
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const uint64_t class_id_mask = (uint64_t{1} << type_arguments->class_id_tag_bits) - 1;
    const uint32_t cid =
        static_cast<uint32_t>((tags >> type_arguments->class_id_tag_shift) & class_id_mask);
    if (cid != type_arguments->cid) return DARTPLANT_OBJECT_HANDLE_INVALID;

    uint32_t length_raw = 0;
    if (!ReadSelf(object + type_arguments->length, &length_raw)) {
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    if ((length_raw & type_arguments->smi_tag_mask) != 0 ||
        (length_raw >> type_arguments->smi_tag_shift) <= index) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (type_arguments->compressed_word_size != sizeof(uint32_t) ||
        index >
            (std::numeric_limits<uintptr_t>::max() - type_arguments->types) / sizeof(uint32_t)) {
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint32_t compressed_element = 0;
    const uintptr_t element_address =
        object + type_arguments->types + static_cast<uintptr_t>(index) * sizeof(uint32_t);
    if (!ReadSelf(element_address, &compressed_element)) {
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    if ((compressed_element & type_arguments->smi_tag_mask) != type_arguments->heap_object_tag) {
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    if (heap_base > std::numeric_limits<uintptr_t>::max() - compressed_element) {
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    *out_raw = heap_base + compressed_element;
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
    const AdapterProfile* profile = nullptr;
    for (const auto& candidate : kProfiles) {
        if (std::strcmp(snapshot_hash, candidate.snapshot_hash) == 0) {
            profile = &candidate;
            break;
        }
    }
    if (profile == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "unsupported snapshot fingerprint: %s",
                            snapshot_hash);
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!ExactFlutterBuildMatches(profile->flutter_build_id)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "unsupported Flutter build-id for Dart %s / Flutter %s",
                            profile->dart_version, profile->flutter_version);
        return DARTPLANT_PROFILE_MISMATCH;
    }
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
    __android_log_print(ANDROID_LOG_INFO, kTag, "current isolate=%p", isolate);
    if (!SelfTestPersistentApi()) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Dart persistent handle API self-test failed");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "persistent API self-test passed");
    auto* instance = new (std::nothrow) FlutterVmAdapterImpl;
    if (instance == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "adapter allocation failed status=%d",
                            DARTPLANT_VM_ADAPTER_BUSY);
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    State& state = instance->state;
    state.profile = profile;
    state.thread = thread;
    const uint64_t thread_isolate = ThreadWord(state, profile->layout.isolate);
    const uint64_t group = ThreadWord(state, profile->layout.isolate_group);
    if (thread_isolate != reinterpret_cast<uint64_t>(isolate) || group == 0) {
        __android_log_print(
            ANDROID_LOG_ERROR, kTag,
            "thread identity mismatch thread_isolate=0x%llx isolate=%p group=0x%llx",
            static_cast<unsigned long long>(thread_isolate), isolate,
            static_cast<unsigned long long>(group));
        delete instance;
        __android_log_print(ANDROID_LOG_ERROR, kTag, "returning isolate mismatch status=%d",
                            DARTPLANT_VM_ISOLATE_MISMATCH);
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    state.enter_safepoint = ResolveStubEntry(state, profile->layout.enter_safepoint_stub);
    state.exit_safepoint = ResolveStubEntry(state, profile->layout.exit_safepoint_stub);
    if (state.enter_safepoint == 0 || state.exit_safepoint == 0) {
        delete instance;
        __android_log_print(ANDROID_LOG_ERROR, kTag, "returning bridge unavailable status=%d",
                            DARTPLANT_VM_BRIDGE_UNAVAILABLE);
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
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
        .isolate = reinterpret_cast<uint64_t>(isolate),
        .isolate_group = group,
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
        ANDROID_LOG_INFO, kTag, "exact Dart %s V3 adapter initialized snapshot=%s thread=0x%llx",
        profile->dart_version, snapshot_hash, static_cast<unsigned long long>(thread));
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
    DartPlantStatus status = dartplant_vm_adapter_detach_isolate(state.adapter, &state.identity);
    if (status != DARTPLANT_OK) return status;
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

const DartPlantFlutterVmDescriptor* DescriptorAt(uint32_t index) {
    static const DartPlantFlutterVmDescriptor descriptors[] = {
        {
            .struct_size = sizeof(DartPlantFlutterVmDescriptor),
            .descriptor_version = 1,
            .vm_adapter_version = 3,
            .descriptor_id = "flutter-3.22.3-dart-3.4.4-android-arm64-product",
            .dart_version = "3.4.4",
            .flutter_version = "3.22.3",
            .snapshot_hash = "d20a1be77c3d3c41b2a5accaee1ce549",
            .flutter_module_name = "libflutter.so",
            .flutter_build_id = "b4ba48b16f1760763d444d35d5d0106ba554fabd",
            .pointer_size = 8,
            .compressed_pointers = 1,
            .product_mode = 1,
            .reserved = {0, 0},
        },
#if !defined(DARTPLANT_FLUTTER_VM_PROFILE_3_4_4)
        {
            .struct_size = sizeof(DartPlantFlutterVmDescriptor),
            .descriptor_version = 1,
            .vm_adapter_version = 3,
            .descriptor_id = "flutter-3.24.0-dart-3.5.0-android-arm64-product",
            .dart_version = "3.5.0",
            .flutter_version = "3.24.0",
            .snapshot_hash = "80a49c7111088100a233b2ae788e1f48",
            .flutter_module_name = "libflutter.so",
            .flutter_build_id = "d9a7f562e5595e9913262ba4b5026a0df242ccfd",
            .pointer_size = 8,
            .compressed_pointers = 1,
            .product_mode = 1,
            .reserved = {0, 0},
        },
        {
            .struct_size = sizeof(DartPlantFlutterVmDescriptor),
            .descriptor_version = 1,
            .vm_adapter_version = 3,
            .descriptor_id = "flutter-3.44.1-dart-3.12.1-android-arm64-product",
            .dart_version = "3.12.1",
            .flutter_version = "3.44.1",
            .snapshot_hash = "ace654289f5abc240509fc941453ebc5",
            .flutter_module_name = "libflutter.so",
            .flutter_build_id = "ca4618220c6646c3546f020f587fef1c75e3c505",
            .pointer_size = 8,
            .compressed_pointers = 1,
            .product_mode = 1,
            .reserved = {0, 0},
        },
#endif
    };
    return index < std::size(descriptors) ? &descriptors[index] : nullptr;
}

uint32_t dartplant_flutter_vm_descriptor_count(void) { return std::size(kProfiles); }

const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_descriptor_at(uint32_t index) {
    return DescriptorAt(index);
}

const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_adapter_descriptor(
    const DartPlantFlutterVmAdapter* instance) {
    if (instance == nullptr) return nullptr;
    const auto* impl = reinterpret_cast<const FlutterVmAdapterImpl*>(instance);
    if (impl->state.profile == nullptr) return nullptr;
    const uint32_t index = static_cast<uint32_t>(impl->state.profile - kProfiles);
    return DescriptorAt(index);
}
