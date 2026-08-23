// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_RUNTIME_VM_ADAPTER_INTERNAL_H_
#define DARTPLANT_RUNTIME_VM_ADAPTER_INTERNAL_H_

#include <stdint.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "dartplant/vm_adapter.h"

struct DartPlantVmAdapter {
    mutable std::mutex mutex;
    DartPlantVmAdapterCallbacks callbacks{};
    void* user_data = nullptr;
    DartPlantIsolateIdentity isolate{};
    std::thread::id owner_thread;
    uint32_t entered = 0;
    uint64_t live_handles = 0;
    uint64_t hook_refs = 0;
    std::vector<DartPlantObjectHandle*> released_handles;
    bool attached = false;
    bool isolate_entered = false;
};

struct DartPlantObjectHandle {
    DartPlantVmAdapter* adapter = nullptr;
    uint32_t adapter_version = 0;
    DartPlantIsolateIdentity isolate{};
    DartPlantObjectStrength strength = DARTPLANT_OBJECT_STRONG;
    DartPlantObjectKind kind = DARTPLANT_OBJECT_UNKNOWN;
    void* backend_handle = nullptr;
    bool released = false;
};

namespace dartplant {

bool VmAdapterIsEntered(DartPlantVmAdapter* adapter);
void VmAdapterRetainHook(DartPlantVmAdapter* adapter);
void VmAdapterReleaseHook(DartPlantVmAdapter* adapter);
DartPlantStatus VmAdapterRetainObject(DartPlantVmAdapter* adapter, uint64_t raw,
                                      DartPlantObjectStrength strength,
                                      DartPlantObjectHandle** out_handle);
DartPlantStatus VmAdapterSetRaw(const DartPlantObjectHandle* handle, uint64_t* out_raw);
DartPlantStatus VmAdapterCheckHandle(const DartPlantVmAdapter* adapter,
                                     const DartPlantObjectHandle* handle);

}  // namespace dartplant

#endif  // DARTPLANT_RUNTIME_VM_ADAPTER_INTERNAL_H_
