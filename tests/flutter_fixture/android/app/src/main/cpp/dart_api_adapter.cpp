#include "dart_api_adapter.h"

#include <android/log.h>
#include <dlfcn.h>

#include <atomic>

#include "dart_api_dl.h"

namespace {

struct RealHandle {
    Dart_PersistentHandle strong = nullptr;
    Dart_WeakPersistentHandle weak = nullptr;
    DartPlantObjectStrength strength = DARTPLANT_OBJECT_STRONG;
    std::atomic<bool> alive = true;
};

struct State {
    DartPlantVmAdapter* adapter = nullptr;
    DartPlantIsolateIdentity identity{};
};

State g_state;
constexpr char kTag[] = "DartPlantFixture";
using DartHandlePredicate = bool (*)(Dart_Handle);
DartHandlePredicate g_is_boolean = nullptr;
DartHandlePredicate g_is_integer = nullptr;
DartHandlePredicate g_is_double = nullptr;
DartHandlePredicate g_is_string = nullptr;

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
    const DartPlantRawToHandle raw_to_handle = dartplant_fixture_raw_to_handle();
    uint64_t group = 0;
    uint64_t generation = 0;
    if (raw_to_handle == nullptr || out_backend == nullptr ||
        !dartplant_fixture_current_group_identity(&group, &generation) || group == 0 ||
        generation == 0) {
        // The public Dart API has no ObjectPtr/raw-word -> Dart_Handle
        // operation. A Flutter engine shim must provide this conversion.
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    Dart_Handle local = raw_to_handle(raw);
    if (local == nullptr) return DARTPLANT_OBJECT_HANDLE_INVALID;
    auto* handle = new RealHandle;
    handle->strength = strength;
    if (strength == DARTPLANT_OBJECT_WEAK) {
        handle->weak = Dart_NewWeakPersistentHandle_DL(local, handle, 0, WeakFinalizer);
        if (handle->weak == nullptr) {
            delete handle;
            return DARTPLANT_OBJECT_HANDLE_INVALID;
        }
    } else {
        handle->strong = Dart_NewPersistentHandle_DL(local);
        if (handle->strong == nullptr) {
            delete handle;
            return DARTPLANT_OBJECT_HANDLE_INVALID;
        }
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
        Dart_DeletePersistentHandle_DL(handle->strong);
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
    const DartPlantHandleToRaw handle_to_raw = dartplant_fixture_handle_to_raw();
    if (backend == nullptr || out_raw == nullptr || handle_to_raw == nullptr) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    uint64_t group = 0;
    uint64_t generation = 0;
    if (!dartplant_fixture_current_group_identity(&group, &generation) || group == 0 ||
        generation == 0) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    auto* handle = static_cast<RealHandle*>(backend);
    Dart_Handle local = handle->strength == DARTPLANT_OBJECT_WEAK
                            ? Dart_HandleFromWeakPersistent_DL(handle->weak)
                            : Dart_HandleFromPersistent_DL(handle->strong);
    if (local == nullptr || (handle->strength == DARTPLANT_OBJECT_WEAK && Dart_IsNull_DL(local))) {
        return DARTPLANT_OBJECT_COLLECTED;
    }
    *out_raw = handle_to_raw(local);
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

const DartPlantVmAdapterCallbacks kCallbacks = {
    .struct_size = sizeof(DartPlantVmAdapterCallbacks),
    .adapter_version = 1,
    .enter_isolate = EnterIsolate,
    .leave_isolate = LeaveIsolate,
    .enter_scope = EnterScope,
    .leave_scope = LeaveScope,
    .retain_object = RetainObject,
    .release_object = ReleaseObject,
    .object_kind = ObjectKind,
    .object_to_raw = ObjectToRaw,
    .object_is_alive = ObjectAlive,
};

}  // namespace

DartPlantStatus dartplant_fixture_create_dart_api_adapter(void* api_dl_data,
                                                          DartPlantVmAdapter** out_adapter) {
    if (api_dl_data == nullptr || out_adapter == nullptr) return DARTPLANT_INVALID_ARGUMENT;
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
    const bool raw_bridge_ready = dartplant_fixture_initialize_raw_bridge();

    uint64_t group = 0;
    uint64_t generation = 0;
    const bool has_identity = dartplant_fixture_current_group_identity(&group, &generation);
    const DartPlantIsolateIdentity identity = {
        .isolate = reinterpret_cast<uint64_t>(isolate),
        .isolate_group = has_identity ? group : 0,
        .generation = has_identity ? generation : 0,
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
                        "Dart API DL adapter initialized; raw bridge=%s build-id=%s engine=%s",
                        raw_bridge_ready ? dartplant_fixture_raw_bridge_backend()
                                         : "unavailable-engine-shim-required",
                        dartplant_fixture_raw_bridge_build_id(),
                        dartplant_fixture_raw_bridge_engine_revision());
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "isolate identity backend=%s isolate=0x%llx group=0x%llx generation=%llu",
                        has_identity ? dartplant_fixture_current_identity_backend() : "unavailable",
                        static_cast<unsigned long long>(identity.isolate),
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
        g_state.adapter = nullptr;
        dartplant_fixture_shutdown_raw_bridge();
    }
    return status;
}

DartPlantVmAdapter* dartplant_fixture_dart_api_adapter() { return g_state.adapter; }

bool dartplant_fixture_dart_api_adapter_ready_for_hooks() {
    return g_state.adapter != nullptr && dartplant_fixture_raw_to_handle() != nullptr &&
           dartplant_fixture_handle_to_raw() != nullptr;
}

void dartplant_fixture_set_raw_handle_bridge(DartPlantRawToHandle raw_to_handle,
                                             DartPlantHandleToRaw handle_to_raw,
                                             DartPlantCurrentIdentity current_identity) {
    dartplant_fixture_set_engine_raw_handle_shim(raw_to_handle, handle_to_raw, current_identity);
}
